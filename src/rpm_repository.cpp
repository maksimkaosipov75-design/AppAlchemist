#include "rpm_repository.h"
#include "utils.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QXmlStreamReader>

#include <zlib.h>

namespace {

// The mirror redirector: it resolves to a mirror close to whoever is running
// the conversion, so no mirror is hard-coded here.
constexpr const char* kFedoraMirror = "https://download.fedoraproject.org/pub/fedora/linux";

QString downloadTool() {
    const QString curl = QStandardPaths::findExecutable("curl");
    if (!curl.isEmpty()) {
        return curl;
    }
    return QStandardPaths::findExecutable("wget");
}

} // namespace

RpmRepository::RpmRepository(QObject* parent)
    : QObject(parent) {
}

void RpmRepository::setRelease(const QString& release) {
    m_release = release.trimmed();
}

void RpmRepository::setArchitecture(const QString& architecture) {
    if (!architecture.trimmed().isEmpty()) {
        m_architecture = architecture.trimmed();
    }
}

QString RpmRepository::releaseFromRpmTag(const QString& rpmRelease) {
    static const QRegularExpression pattern(QStringLiteral("fc(\\d+)"));
    const QRegularExpressionMatch match = pattern.match(rpmRelease);
    return match.hasMatch() ? match.captured(1) : QString();
}

QString RpmRepository::baseUrl() const {
    return QString("%1/releases/%2/Everything/%3/os")
        .arg(kFedoraMirror, m_release, m_architecture);
}

QString RpmRepository::cacheDirectory() const {
    const QString root = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    return QString("%1/rpm-repository/%2-%3").arg(root, m_release, m_architecture);
}

bool RpmRepository::fetch(const QString& url, const QString& destination, int timeoutMs) {
    const QString tool = downloadTool();
    if (tool.isEmpty()) {
        emit log("Neither curl nor wget is available; cannot read the repository");
        return false;
    }

    QDir().mkpath(QFileInfo(destination).absolutePath());
    const QString staging = destination + ".part";
    QFile::remove(staging);

    QStringList arguments;
    if (tool.endsWith("curl")) {
        arguments << "-sSL" << "--fail" << "--max-time" << QString::number(timeoutMs / 1000)
                  << "-o" << staging << url;
    } else {
        arguments << "-q" << "--tries=2" << "--timeout=" + QString::number(timeoutMs / 1000)
                  << "-O" << staging << url;
    }

    const ProcessResult result = SubprocessWrapper::execute(tool, arguments, {}, timeoutMs);
    if (!result.success || QFileInfo(staging).size() <= 0) {
        // Without the reason a failure here reads as "the repository is
        // unreachable", which hides an expired release, a proxy or a full disk.
        const QString reason = result.stderrOutput.trimmed().isEmpty()
                                   ? QString("exit code %1").arg(result.exitCode)
                                   : result.stderrOutput.trimmed().left(200);
        emit log(QString("Could not download %1: %2").arg(QFileInfo(destination).fileName(), reason));
        QFile::remove(staging);
        return false;
    }

    QFile::remove(destination);
    return QFile::rename(staging, destination);
}

bool RpmRepository::ensureMetadata() {
    if (!isUsable()) {
        return false;
    }

    const QString cacheDir = cacheDirectory();
    QDir().mkpath(cacheDir);

    // The index location carries its own checksum, so a cached copy under that
    // name is current by construction and nothing has to be revalidated.
    const QString repomd = cacheDir + "/repomd.xml";
    if (!fetch(baseUrl() + "/repodata/repomd.xml", repomd, 60000)) {
        emit log(QString("Could not read the package index for Fedora %1").arg(m_release));
        return false;
    }

    QFile file(repomd);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QString content = QString::fromUtf8(file.readAll());
    file.close();

    static const QRegularExpression primaryPattern(
        QStringLiteral("<data type=\"primary\">.*?<location href=\"([^\"]+)\""),
        QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch match = primaryPattern.match(content);
    if (!match.hasMatch()) {
        emit log("The package index does not name its primary metadata");
        return false;
    }

    m_primaryPath = match.captured(1);
    m_primaryFile = cacheDir + "/" + QFileInfo(m_primaryPath).fileName();

    if (QFileInfo::exists(m_primaryFile) && QFileInfo(m_primaryFile).size() > 0) {
        emit log("Using the cached package index");
        return true;
    }

    emit log(QString("Downloading the package index of Fedora %1 (about 19 MB, kept for later conversions)")
                 .arg(m_release));
    if (!fetch(baseUrl() + "/" + m_primaryPath, m_primaryFile, 600000)) {
        emit log("Could not download the package index");
        return false;
    }
    return true;
}

QHash<QString, QString> RpmRepository::resolve(const QSet<QString>& sonames,
                                               const QSet<QString>& packageNames) {
    QHash<QString, QString> found;
    if (m_primaryFile.isEmpty() || (sonames.isEmpty() && packageNames.isEmpty())) {
        return found;
    }

    gzFile index = gzopen(QFile::encodeName(m_primaryFile).constData(), "rb");
    if (index == nullptr) {
        emit log("Could not open the cached package index");
        return found;
    }

    QXmlStreamReader reader;
    QString currentName;
    QString currentLocation;
    QString currentArch;
    QStringList currentProvides;
    bool inProvides = false;
    int matched = 0;

    QByteArray buffer;
    buffer.resize(1 << 20);

    // A repository carries the same library for several architectures. Taking
    // the first match would drop a 32-bit library into a 64-bit bundle, where
    // the loader rejects it outright, so the decision waits until the package
    // has named its architecture.
    const QString wantedArch = m_architecture;

    auto commitPackage = [&]() {
        if (currentLocation.isEmpty()) {
            return;
        }
        if (currentArch != wantedArch && currentArch != QLatin1String("noarch")) {
            return;
        }
        for (const QString& provided : currentProvides) {
            if (!found.contains(provided)) {
                found.insert(provided, currentLocation);
                matched++;
            }
        }
        if (!currentName.isEmpty() && packageNames.contains(currentName) &&
            !found.contains(currentName)) {
            found.insert(currentName, currentLocation);
            matched++;
        }
    };

    auto handleToken = [&]() {
        if (reader.isStartElement()) {
            const QStringView element = reader.name();
            if (element == QLatin1String("package")) {
                currentName.clear();
                currentLocation.clear();
                currentArch.clear();
                currentProvides.clear();
                inProvides = false;
            } else if (element == QLatin1String("name") && currentName.isEmpty()) {
                currentName = reader.readElementText();
            } else if (element == QLatin1String("arch") && currentArch.isEmpty()) {
                currentArch = reader.readElementText();
            } else if (element == QLatin1String("location")) {
                currentLocation = reader.attributes().value("href").toString();
            } else if (element == QLatin1String("provides")) {
                inProvides = true;
            } else if (element == QLatin1String("entry") && inProvides) {
                QString provided = reader.attributes().value("name").toString();
                // Provides carry decorations such as "libfoo.so.1()(64bit)".
                const int decoration = provided.indexOf('(');
                if (decoration > 0) {
                    provided = provided.left(decoration);
                }
                if (sonames.contains(provided) && !currentProvides.contains(provided)) {
                    currentProvides << provided;
                }
            }
        } else if (reader.isEndElement()) {
            if (reader.name() == QLatin1String("provides")) {
                inProvides = false;
            } else if (reader.name() == QLatin1String("package")) {
                commitPackage();
            }
        }
    };

    forever {
        const int read = gzread(index, buffer.data(), buffer.size());
        if (read <= 0) {
            break;
        }
        reader.addData(QByteArray(buffer.constData(), read));
        while (!reader.atEnd()) {
            reader.readNext();
            if (reader.hasError()) {
                break;
            }
            handleToken();
        }
        if (reader.error() != QXmlStreamReader::NoError &&
            reader.error() != QXmlStreamReader::PrematureEndOfDocumentError) {
            emit log(QString("The package index could not be read: %1").arg(reader.errorString()));
            break;
        }
    }
    gzclose(index);

    emit log(QString("Found %1 of %2 wanted packages in the index")
                 .arg(matched)
                 .arg(sonames.size() + packageNames.size()));
    return found;
}

QStringList RpmRepository::download(const QStringList& locations, const QString& targetDir) {
    QStringList downloaded;
    if (locations.isEmpty()) {
        return downloaded;
    }
    QDir().mkpath(targetDir);

    for (const QString& location : locations) {
        const QString fileName = QFileInfo(location).fileName();
        const QString destination = QString("%1/%2").arg(targetDir, fileName);
        if (QFileInfo::exists(destination)) {
            downloaded << destination;
            continue;
        }
        if (fetch(baseUrl() + "/" + location, destination, 300000)) {
            downloaded << destination;
        } else {
            emit log(QString("Could not download %1").arg(fileName));
        }
    }

    return downloaded;
}
