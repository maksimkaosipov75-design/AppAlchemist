#include "repository_browser.h"
#include "repository_search_worker.h"

#include <QDir>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QFileInfo>
#include <QDateTime>

namespace {
QString commandOutput(const QString& program, const QStringList& args, int timeoutMs = 60000) {
    QProcess process;
    process.start(program, args);
    if (!process.waitForStarted(5000)) {
        return QString();
    }
    process.waitForFinished(timeoutMs);
    return QString::fromUtf8(process.readAllStandardOutput());
}

QStringList commandOutputLines(const QString& program, const QStringList& args, int timeoutMs = 60000) {
    return commandOutput(program, args, timeoutMs).split('\n', Qt::SkipEmptyParts);
}

QString newestMatchingFile(const QString& dirPath, const QString& pattern) {
    QDir dir(dirPath);
    const QFileInfoList files = dir.entryInfoList(
        QStringList() << pattern,
        QDir::Files,
        QDir::Time | QDir::Reversed);
    if (files.isEmpty()) {
        return QString();
    }
    return files.last().absoluteFilePath();
}
} // namespace

RepositoryBrowser::RepositoryBrowser(QObject* parent)
    : QObject(parent)
    , m_networkManager(new QNetworkAccessManager(this))
    , m_currentDownload(nullptr)
    , m_cancelled(0)
    , m_searchThread(nullptr)
    , m_searchWorker(nullptr) {
}

RepositoryBrowser::~RepositoryBrowser() {
    if (m_searchThread) {
        m_searchThread->quit();
        m_searchThread->wait();
    }
}

PackageManager RepositoryBrowser::detectPackageManager() {
    if (!QStandardPaths::findExecutable("apt-cache").isEmpty()) {
        return PackageManager::APT;
    }
    if (!QStandardPaths::findExecutable("dnf").isEmpty()) {
        return PackageManager::DNF;
    }
    if (!QStandardPaths::findExecutable("pacman").isEmpty()) {
        return PackageManager::PACMAN;
    }
    if (!QStandardPaths::findExecutable("zypper").isEmpty()) {
        return PackageManager::ZYPPER;
    }
    return PackageManager::UNKNOWN;
}

QString RepositoryBrowser::packageManagerName(PackageManager pm) {
    switch (pm) {
        case PackageManager::APT:
            return QStringLiteral("APT");
        case PackageManager::DNF:
            return QStringLiteral("DNF");
        case PackageManager::PACMAN:
            return QStringLiteral("Pacman");
        case PackageManager::ZYPPER:
            return QStringLiteral("Zypper");
        default:
            return QStringLiteral("Unknown");
    }
}

void RepositoryBrowser::searchPackages(const QString& query) {
    m_cancelled.storeRelease(0);
    emit searchStarted();

    const PackageManager pm = detectPackageManager();
    switch (pm) {
        case PackageManager::APT:
            m_searchResults = searchApt(query);
            break;
        case PackageManager::DNF:
            m_searchResults = searchDnf(query);
            break;
        case PackageManager::PACMAN:
            m_searchResults = searchPacman(query);
            break;
        case PackageManager::ZYPPER:
            m_searchResults = searchZypper(query);
            break;
        default:
            emit searchError(QStringLiteral("No supported package manager found"));
            return;
    }

    emit searchCompleted(m_searchResults);
}

void RepositoryBrowser::searchPackagesAsync(const QString& query) {
    if (m_searchThread) {
        m_searchThread->quit();
        m_searchThread->wait();
        m_searchThread->deleteLater();
        m_searchThread = nullptr;
    }

    m_searchThread = new QThread(this);
    m_searchWorker = new RepositorySearchWorker(this);
    m_searchWorker->moveToThread(m_searchThread);

    connect(m_searchThread, &QThread::started, this, [this, query]() {
        if (m_searchWorker) {
            m_searchWorker->performSearch(query);
        }
    });

    connect(m_searchWorker, &RepositorySearchWorker::searchCompleted, this, [this](const QList<PackageInfo>& results) {
        m_searchResults = results;
        emit searchCompleted(results);
        onSearchThreadFinished();
    });

    connect(m_searchWorker, &RepositorySearchWorker::searchError, this, [this](const QString& error) {
        emit searchError(error);
        onSearchThreadFinished();
    });
    connect(m_searchThread, &QThread::finished, m_searchWorker, &QObject::deleteLater);

    m_searchThread->start();
}

void RepositoryBrowser::downloadPackage(const PackageInfo& package, const QString& outputDir) {
    m_cancelled.storeRelease(0);
    emit downloadStarted(package.name);

    const QDir outDir(outputDir);
    if (!outDir.exists()) {
        QDir().mkpath(outputDir);
    }

    bool ok = false;
    QString outputPath;

    switch (detectPackageManager()) {
        case PackageManager::APT:
            ok = downloadApt(package, outputDir);
            outputPath = newestMatchingFile(outputDir, QString("%1*.deb").arg(package.name));
            break;
        case PackageManager::DNF:
            ok = downloadDnf(package, outputDir);
            outputPath = newestMatchingFile(outputDir, QString("%1*.rpm").arg(package.name));
            break;
        case PackageManager::PACMAN:
            ok = downloadPacman(package, outputDir);
            outputPath = newestMatchingFile(outputDir, QString("%1*.pkg.tar.*").arg(package.name));
            break;
        default:
            emit downloadError(QStringLiteral("Unsupported package manager for download"));
            return;
    }

    if (!ok || outputPath.isEmpty()) {
        emit downloadError(QString("Failed to download package: %1").arg(package.name));
        return;
    }

    emit downloadCompleted(outputPath);
}

void RepositoryBrowser::cancel() {
    m_cancelled.storeRelease(1);
    if (m_currentDownload) {
        m_currentDownload->abort();
    }
    if (m_searchThread && m_searchThread->isRunning()) {
        m_searchThread->quit();
    }
}

void RepositoryBrowser::onDownloadProgress(qint64 bytesReceived, qint64 bytesTotal) {
    emit downloadProgress(bytesReceived, bytesTotal);
}

void RepositoryBrowser::onDownloadFinished() {
    if (!m_currentDownload) {
        return;
    }

    if (m_currentDownload->error() != QNetworkReply::NoError) {
        emit downloadError(m_currentDownload->errorString());
    }

    m_currentDownload->deleteLater();
    m_currentDownload = nullptr;
}

void RepositoryBrowser::onDownloadError(QNetworkReply::NetworkError error) {
    Q_UNUSED(error);
    if (m_currentDownload) {
        emit downloadError(m_currentDownload->errorString());
    }
}

void RepositoryBrowser::onSearchThreadFinished() {
    if (!m_searchThread) {
        return;
    }

    m_searchThread->quit();
    m_searchThread->wait();
    m_searchThread->deleteLater();
    m_searchThread = nullptr;
    m_searchWorker = nullptr;
}

QList<PackageInfo> RepositoryBrowser::searchApt(const QString& query, bool silent) {
    QList<PackageInfo> results;

    const QStringList lines = commandOutputLines("apt-cache", {"search", query}, 120000);
    for (const QString& line : lines) {
        if (m_cancelled.loadAcquire() != 0) {
            break;
        }

        const int sep = line.indexOf(" - ");
        if (sep <= 0) {
            continue;
        }

        PackageInfo p;
        p.name = line.left(sep).trimmed();
        p.description = line.mid(sep + 3).trimmed();
        p.repository = QStringLiteral("apt");
        p.source = PackageManager::APT;

        if (!p.name.isEmpty()) {
            results.append(p);
        }

        if (results.size() >= 200) {
            break;
        }
    }

    if (!silent) {
        emit log(QString("APT search found %1 packages for query '%2'").arg(results.size()).arg(query));
    }

    return results;
}

QList<PackageInfo> RepositoryBrowser::searchDnf(const QString& query, bool silent) {
    QList<PackageInfo> results;
    const QStringList lines = commandOutputLines("dnf", {"search", query}, 120000);

    QRegularExpression rx(R"(^([^\s:]+)\s*:\s*(.+)$)");
    for (const QString& line : lines) {
        if (m_cancelled.loadAcquire() != 0) {
            break;
        }
        const auto match = rx.match(line.trimmed());
        if (!match.hasMatch()) {
            continue;
        }

        PackageInfo p;
        p.name = match.captured(1).trimmed();
        p.description = match.captured(2).trimmed();
        p.repository = QStringLiteral("dnf");
        p.source = PackageManager::DNF;

        if (!p.name.isEmpty()) {
            results.append(p);
        }

        if (results.size() >= 200) {
            break;
        }
    }

    if (!silent) {
        emit log(QString("DNF search found %1 packages for query '%2'").arg(results.size()).arg(query));
    }

    return results;
}

QList<PackageInfo> RepositoryBrowser::searchPacman(const QString& query, bool silent) {
    QList<PackageInfo> results;
    const QStringList lines = commandOutputLines("pacman", {"-Ss", query}, 120000);

    QRegularExpression rx(R"(^([^/]+)/([^\s]+)\s+(.+)$)");
    for (const QString& line : lines) {
        if (m_cancelled.loadAcquire() != 0) {
            break;
        }

        const auto m = rx.match(line.trimmed());
        if (!m.hasMatch()) {
            continue;
        }

        PackageInfo p;
        p.repository = m.captured(1);
        p.name = m.captured(2);
        p.version = m.captured(3);
        p.source = PackageManager::PACMAN;

        results.append(p);
        if (results.size() >= 200) {
            break;
        }
    }

    if (!silent) {
        emit log(QString("Pacman search found %1 packages for query '%2'").arg(results.size()).arg(query));
    }

    return results;
}

QList<PackageInfo> RepositoryBrowser::searchZypper(const QString& query, bool silent) {
    QList<PackageInfo> results;
    const QStringList lines = commandOutputLines("zypper", {"search", query}, 120000);

    for (const QString& rawLine : lines) {
        if (m_cancelled.loadAcquire() != 0) {
            break;
        }

        const QString line = rawLine.trimmed();
        if (!line.contains('|')) {
            continue;
        }

        const QStringList cols = line.split('|', Qt::SkipEmptyParts);
        if (cols.size() < 3) {
            continue;
        }

        PackageInfo p;
        p.name = cols.at(1).trimmed();
        p.description = cols.last().trimmed();
        p.repository = QStringLiteral("zypper");
        p.source = PackageManager::ZYPPER;

        if (!p.name.isEmpty() && p.name != QStringLiteral("Name")) {
            results.append(p);
        }

        if (results.size() >= 200) {
            break;
        }
    }

    if (!silent) {
        emit log(QString("Zypper search found %1 packages for query '%2'").arg(results.size()).arg(query));
    }

    return results;
}

bool RepositoryBrowser::downloadApt(const PackageInfo& package, const QString& outputDir) {
    QProcess process;
    process.setProgram("bash");
    process.setArguments({"-lc", QString("cd '%1' && apt download '%2'").arg(outputDir, package.name)});
    process.start();
    process.waitForFinished(180000);
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

bool RepositoryBrowser::downloadDnf(const PackageInfo& package, const QString& outputDir) {
    QProcess process;
    process.setProgram("dnf");
    process.setArguments({"download", "--destdir", outputDir, package.name});
    process.start();
    process.waitForFinished(300000);
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

bool RepositoryBrowser::downloadPacman(const PackageInfo& package, const QString& outputDir) {
    Q_UNUSED(outputDir);
    emit log(QString("Pacman download for '%1' is not implemented in this build path").arg(package.name));
    return false;
}

PackageInfo RepositoryBrowser::parseAptShowOutput(const QString& output) {
    PackageInfo p;
    const QStringList lines = output.split('\n');
    for (const QString& line : lines) {
        if (line.startsWith("Package:")) {
            p.name = line.mid(QString("Package:").size()).trimmed();
        } else if (line.startsWith("Version:")) {
            p.version = line.mid(QString("Version:").size()).trimmed();
        } else if (line.startsWith("Description:")) {
            p.description = line.mid(QString("Description:").size()).trimmed();
        } else if (line.startsWith("Architecture:")) {
            p.architecture = line.mid(QString("Architecture:").size()).trimmed();
        }
    }
    p.source = PackageManager::APT;
    return p;
}

PackageInfo RepositoryBrowser::parseDnfInfoOutput(const QString& output) {
    PackageInfo p;
    const QStringList lines = output.split('\n');
    for (const QString& line : lines) {
        if (line.startsWith("Name")) {
            const int idx = line.indexOf(':');
            if (idx > 0) p.name = line.mid(idx + 1).trimmed();
        } else if (line.startsWith("Version")) {
            const int idx = line.indexOf(':');
            if (idx > 0) p.version = line.mid(idx + 1).trimmed();
        } else if (line.startsWith("Arch")) {
            const int idx = line.indexOf(':');
            if (idx > 0) p.architecture = line.mid(idx + 1).trimmed();
        } else if (line.startsWith("Summary")) {
            const int idx = line.indexOf(':');
            if (idx > 0) p.description = line.mid(idx + 1).trimmed();
        }
    }
    p.source = PackageManager::DNF;
    return p;
}
