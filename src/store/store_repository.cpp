#include "store/store_repository.h"
#include <QRegularExpression>
#include <QEventLoop>
#include <QTimer>
#include "store/name_trace.h"

StoreRepository::StoreRepository(QObject* parent)
    : QObject(parent)
    , m_appStream(new AppStreamMetadata(this))
    , m_repoBrowser(new RepositoryBrowser(this)) {
    connect(m_repoBrowser, &RepositoryBrowser::searchCompleted,
            this, &StoreRepository::onRepoSearchCompleted);
    connect(m_repoBrowser, &RepositoryBrowser::downloadCompleted,
            this, &StoreRepository::downloadCompleted);
    connect(m_repoBrowser, &RepositoryBrowser::downloadError,
            this, &StoreRepository::downloadError);
    connect(m_repoBrowser, &RepositoryBrowser::downloadProgress,
            this, &StoreRepository::downloadProgress);
}

QList<StoreAppEntry> StoreRepository::loadAppStreamApps() {
    QList<StoreAppEntry> entries;
    QList<AppInfo> apps = m_appStream->loadAllApps();
    entries.reserve(apps.size());
    for (const AppInfo& app : apps) {
        QString key = !app.packageName.isEmpty() ? app.packageName : app.appId;
        StoreNameTrace::trace("repo-load-appstream", key, app.displayName);
        entries.append(fromAppInfo(app));
    }
    return entries;
}

QList<StoreAppEntry> StoreRepository::searchRepositories(const QString& query, int limit) {
    // This method is kept for synchronous fallback, but should use async version
    // For now, we'll use async and wait (not ideal, but maintains compatibility)
    QList<StoreAppEntry> entries;
    m_repoBrowser->searchPackagesAsync(query);
    
    // Wait for results with timeout
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    timeout.setInterval(10000); // 10 second timeout
    
    connect(m_repoBrowser, &RepositoryBrowser::searchCompleted, &loop, &QEventLoop::quit);
    connect(m_repoBrowser, &RepositoryBrowser::searchError, &loop, &QEventLoop::quit);
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    
    timeout.start();
    loop.exec();
    
    QList<PackageInfo> results = m_repoBrowser->searchResults();
    for (const PackageInfo& pkg : results) {
        if (entries.size() >= limit) break;
        entries.append(fromPackageInfo(pkg));
    }
    return entries;
}

void StoreRepository::searchRepositoriesAsync(const QString& query, int limit) {
    m_searchLimit = limit;
    emit searchStarted();
    m_repoBrowser->searchPackagesAsync(query);
}

QPixmap StoreRepository::loadIcon(const StoreAppEntry& app, int size) {
    if (!app.iconPath.isEmpty()) {
        QPixmap icon(app.iconPath);
        if (!icon.isNull()) {
            return icon.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
    }
    QString lookup = !app.packageName.isEmpty() ? app.packageName : app.appId;
    if (lookup.isEmpty()) {
        return QPixmap();
    }
    QPixmap icon = m_appStream->findIconForPackage(lookup, app.appId, app.desktopId, size);
    if (icon.isNull()) {
        icon = m_appStream->getIcon(lookup, size);
    }
    return icon;
}

void StoreRepository::downloadPackage(const StoreAppEntry& app, const QString& outputDir) {
    PackageInfo pkg;
    
    // Use packageName if available, otherwise try to extract from appId
    QString packageName = app.packageName;
    if (packageName.isEmpty() && !app.appId.isEmpty()) {
        // Extract package name from appId (e.g., org.videolan.vlc -> vlc)
        QStringList parts = app.appId.split('.', Qt::SkipEmptyParts);
        if (!parts.isEmpty()) {
            packageName = parts.last();
        } else {
            packageName = app.appId;
        }
    }
    
    // If still empty, use appId as last resort
    if (packageName.isEmpty()) {
        packageName = app.appId;
    }
    
    pkg.name = packageName;
    pkg.version = app.version;
    pkg.description = app.summary;
    pkg.size = app.size;
    pkg.repository = app.repository;
    pkg.source = app.source;
    m_repoBrowser->downloadPackage(pkg, outputDir);
}

void StoreRepository::setSudoPassword(const QString& password) {
    m_repoBrowser->setSudoPassword(password);
}

void StoreRepository::onRepoSearchCompleted(QList<PackageInfo> results) {
    QList<StoreAppEntry> entries;
    entries.reserve(qMin(results.size(), m_searchLimit));
    for (const PackageInfo& pkg : results) {
        if (entries.size() >= m_searchLimit) break;
        entries.append(fromPackageInfo(pkg));
    }
    emit searchCompleted(entries);
}

namespace {
// Work with bytes to avoid QString corruption
QString sanitizeText(const QString& input) {
    QByteArray bytes = input.toUtf8();
    QByteArray result;
    result.reserve(bytes.size());
    
    for (int i = 0; i < bytes.size(); ++i) {
        unsigned char ch = bytes.at(i);
        // Remove control characters, keep UTF-8
        if (ch >= 32 || (ch & 0x80)) {
            result.append(ch);
        }
    }
    
    return QString::fromUtf8(result).simplified();
}

QString sanitizePackageName(const QString& input) {
    QByteArray bytes = sanitizeText(input).toUtf8();
    QByteArray result;
    result.reserve(bytes.size());
    
    for (int i = 0; i < bytes.size(); ++i) {
        char ch = bytes.at(i);
        if (ch == ' ') {
            result.append('-');
        } else {
            result.append(ch);
        }
    }
    
    // Remove leading non-alphanumeric
    while (!result.isEmpty() && !QChar(result.at(0)).isLetterOrNumber()) {
        result.remove(0, 1);
    }
    
    return QString::fromUtf8(result);
}

bool looksCorruptDisplayName(const QString& displayName, const QString& packageName) {
    if (displayName.isEmpty() || packageName.isEmpty()) {
        return displayName.isEmpty();
    }
    if (!displayName.at(0).isLetterOrNumber() && packageName.at(0).isLetterOrNumber()) {
        return true;
    }
    if (displayName.size() == packageName.size()) {
        if (displayName.mid(1) == packageName.mid(1) && displayName.at(0) != packageName.at(0)) {
            return true;
        }
    }
    return false;
}
}

StoreAppEntry StoreRepository::fromAppInfo(const AppInfo& app) const {
    StoreAppEntry entry;
    
    // Use QString::fromUtf8 to ensure proper deep copy and avoid string corruption
    QString appIdStr = app.appId.isEmpty() ? app.packageName : app.appId;
    entry.appId = QString::fromUtf8(sanitizeText(appIdStr).toUtf8());
    entry.desktopId = QString::fromUtf8(sanitizeText(app.desktopId).toUtf8());
    
    // Ensure packageName is set - extract from appId if empty
    QString pkgName = app.packageName;
    if (pkgName.isEmpty() && !app.appId.isEmpty()) {
        // Extract package name from appId (e.g., org.videolan.vlc -> vlc)
        QStringList parts = app.appId.split('.', Qt::SkipEmptyParts);
        if (!parts.isEmpty()) {
            pkgName = parts.last();
        } else {
            pkgName = app.appId;
        }
    }
    entry.packageName = QString::fromUtf8(sanitizePackageName(pkgName).toUtf8());
    entry.displayName = QString::fromUtf8(sanitizeText(app.displayName).toUtf8());

    if (looksCorruptDisplayName(entry.displayName, entry.packageName)) {
        QString desktopFile = m_appStream->findDesktopFile(entry.packageName);
        if (desktopFile.isEmpty() && !app.appId.isEmpty()) {
            desktopFile = m_appStream->findDesktopFile(app.appId);
        }
        if (!desktopFile.isEmpty()) {
            QString desktopName = m_appStream->extractNameFromDesktop(desktopFile);
            if (!desktopName.isEmpty()) {
                entry.displayName = QString::fromUtf8(sanitizeText(desktopName).toUtf8());
            }
        }
        if (looksCorruptDisplayName(entry.displayName, entry.packageName) && !app.name.isEmpty()) {
            entry.displayName = QString::fromUtf8(sanitizeText(app.name).toUtf8());
        }
    }

    StoreNameTrace::trace("repo-fromAppInfo", entry.packageName, entry.displayName);
    entry.summary = QString::fromUtf8(sanitizeText(app.description).toUtf8());
    
    QString desc = app.longDescription.isEmpty() ? app.description : app.longDescription;
    entry.description = QString::fromUtf8(sanitizeText(desc).toUtf8());
    
    entry.categories = app.categories;
    entry.iconPath = app.iconPath;
    if (!app.desktopId.isEmpty()) {
        entry.iconKey = app.desktopId;
    } else if (!app.appId.isEmpty()) {
        entry.iconKey = app.appId;
    } else {
        entry.iconKey = app.iconPath.isEmpty() ? entry.packageName : app.iconPath;
    }
    entry.version = QString::fromUtf8(sanitizeText(app.version).toUtf8());
    entry.size = app.size;
    
    // Use system package manager if source is unknown (for AppStream apps)
    if (app.source == PackageManager::UNKNOWN) {
        entry.source = RepositoryBrowser::detectPackageManager();
    } else {
        entry.source = app.source;
    }
    
    entry.repository = QString::fromUtf8(sanitizeText(app.repository).toUtf8());
    entry.releaseDate = app.releaseDate;
    return entry;
}

StoreAppEntry StoreRepository::fromPackageInfo(const PackageInfo& pkg) const {
    StoreAppEntry entry;
    entry.appId = sanitizeText(pkg.name);
    entry.packageName = sanitizePackageName(pkg.name);
    entry.displayName = sanitizeText(pkg.name);
    entry.summary = sanitizeText(pkg.description);
    entry.description = sanitizeText(pkg.description);
    entry.version = sanitizeText(pkg.version);
    entry.size = pkg.size;
    entry.source = pkg.source;
    entry.repository = sanitizeText(pkg.repository);
    entry.categories << "Other";
    return entry;
}

