#ifndef APPSTREAM_METADATA_H
#define APPSTREAM_METADATA_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QPixmap>
#include <QList>
#include <QMap>
#include <QDateTime>
#include "repository_browser.h"

// Extended application information with AppStream metadata
struct AppInfo {
    AppInfo() = default;
    AppInfo(const AppInfo& other)
        : name(deepCopy(other.name))
        , packageName(deepCopy(other.packageName))
        , displayName(deepCopy(other.displayName))
        , appId(deepCopy(other.appId))
        , desktopId(deepCopy(other.desktopId))
        , version(deepCopy(other.version))
        , description(deepCopy(other.description))
        , longDescription(deepCopy(other.longDescription))
        , size(other.size)
        , architecture(deepCopy(other.architecture))
        , repository(deepCopy(other.repository))
        , downloadUrl(deepCopy(other.downloadUrl))
        , source(other.source)
        , icon(other.icon)
        , iconPath(deepCopy(other.iconPath))
        , screenshots(other.screenshots)
        , screenshotPaths(other.screenshotPaths)
        , categories(other.categories)
        , license(deepCopy(other.license))
        , developer(deepCopy(other.developer))
        , developerName(deepCopy(other.developerName))
        , homepage(deepCopy(other.homepage))
        , releaseDate(other.releaseDate)
        , rating(other.rating)
        , isPopular(other.isPopular)
        , isNew(other.isNew)
        , isRecommended(other.isRecommended)
        , downloadCount(other.downloadCount)
        , priority(other.priority) {}

    AppInfo& operator=(const AppInfo& other) {
        if (this != &other) {
            name = deepCopy(other.name);
            packageName = deepCopy(other.packageName);
            displayName = deepCopy(other.displayName);
            appId = deepCopy(other.appId);
            desktopId = deepCopy(other.desktopId);
            version = deepCopy(other.version);
            description = deepCopy(other.description);
            longDescription = deepCopy(other.longDescription);
            size = other.size;
            architecture = deepCopy(other.architecture);
            repository = deepCopy(other.repository);
            downloadUrl = deepCopy(other.downloadUrl);
            source = other.source;
            icon = other.icon;
            iconPath = deepCopy(other.iconPath);
            screenshots = other.screenshots;
            screenshotPaths = other.screenshotPaths;
            categories = other.categories;
            license = deepCopy(other.license);
            developer = deepCopy(other.developer);
            developerName = deepCopy(other.developerName);
            homepage = deepCopy(other.homepage);
            releaseDate = other.releaseDate;
            rating = other.rating;
            isPopular = other.isPopular;
            isNew = other.isNew;
            isRecommended = other.isRecommended;
            downloadCount = other.downloadCount;
            priority = other.priority;
        }
        return *this;
    }

    // Basic package information
    QString name;
    QString packageName;
    QString displayName;
    QString appId;
    QString desktopId;
    QString version;
    QString description;
    QString longDescription;
    qint64 size = 0;
    QString architecture;
    QString repository;
    QString downloadUrl;
    PackageManager source = PackageManager::UNKNOWN;
    
    // AppStream metadata
    QPixmap icon;
    QString iconPath;
    QList<QPixmap> screenshots;
    QStringList screenshotPaths;
    QStringList categories;  // ["AudioVideo", "Development", ...]
    QString license;
    QString developer;
    QString developerName;
    QString homepage;
    QDateTime releaseDate;
    double rating = 0.0;
    
    // Metadata for collections
    bool isPopular = false;
    bool isNew = false;
    bool isRecommended = false;
    int downloadCount = 0;
    int priority = 0;  // Package priority in repository

    // Helper methods
    QString sizeFormatted() const {
        if (size < 1024) return QString("%1 B").arg(size);
        if (size < 1024 * 1024) return QString("%1 KB").arg(size / 1024);
        return QString("%1 MB").arg(size / 1024.0 / 1024.0, 0, 'f', 1);
    }
    
    // Convert from PackageInfo
    static AppInfo fromPackageInfo(const PackageInfo& pkg) {
        AppInfo app;
        app.name = pkg.name;
        app.packageName = pkg.name;
        app.displayName = pkg.name;
        app.version = pkg.version;
        app.description = pkg.description;
        app.size = pkg.size;
        app.architecture = pkg.architecture;
        app.repository = pkg.repository;
        app.downloadUrl = pkg.downloadUrl;
        app.source = pkg.source;
        return app;
    }

private:
    static QString deepCopy(const QString& value) {
        return QString::fromUtf8(value.toUtf8());
    }
};

class AppStreamMetadata : public QObject {
    Q_OBJECT

public:
    explicit AppStreamMetadata(QObject* parent = nullptr);
    ~AppStreamMetadata();
    
    // Load all applications from AppStream
    QList<AppInfo> loadAllApps();
    
    // Get application info by package name
    AppInfo getAppInfo(const QString& packageName);
    
    // Get icon for application
    QPixmap getIcon(const QString& packageName, int size = 128);
    
    // Find icon for package (improved search)
    QPixmap findIconForPackage(const QString& packageName, int size = 128);
    QPixmap findIconForPackage(const QString& packageName, const QString& appId, const QString& desktopId, int size = 128);
    
    // Find desktop file for package
    QString findDesktopFile(const QString& packageName);
    
    // Extract icon name from desktop file
    QString extractIconFromDesktop(const QString& desktopFile);
    
    // Extract application name from desktop file
    QString extractNameFromDesktop(const QString& desktopFile);
    
    // Extract categories from desktop file
    QStringList extractCategoriesFromDesktop(const QString& desktopFile);
    
    // Map repository section to categories
    QStringList categoriesFromSection(const QString& section);
    
    // Get screenshots for application
    QList<QPixmap> getScreenshots(const QString& packageName);
    
    // Get categories for application
    QStringList getCategories(const QString& packageName);
    
    // Check if AppStream is available
    bool isAvailable() const;
    
    // Get category display name
    static QString categoryDisplayName(const QString& category);
    
    // Get all available categories
    static QStringList getAllCategories();

signals:
    void loadingProgress(int current, int total);
    void loadingFinished();
    void loadingError(const QString& error);

private:
    // AppStream XML file paths
    QStringList findAppStreamFiles() const;
    
    // Parse single AppStream XML file
    QList<AppInfo> parseAppStreamFile(const QString& filePath);
    
    // Parse component element from XML
    AppInfo parseComponent(const QString& xmlContent, const QString& filePath);
    
    // Load icon from path
    QPixmap loadIcon(const QString& iconPath, int size = 128);
    
    // Find icon in standard locations
    QString findIconPath(const QString& iconName, int size = 128);
    
    // Cache for loaded apps
    QMap<QString, AppInfo> m_appCache;
    bool m_cacheLoaded;
    
    // Standard AppStream paths
    QStringList m_appStreamPaths;
};

#endif // APPSTREAM_METADATA_H

