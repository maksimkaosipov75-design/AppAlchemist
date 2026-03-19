#ifndef CACHE_MANAGER_H
#define CACHE_MANAGER_H

#include <QString>
#include <QDateTime>
#include <QFileInfo>
#include <QDir>
#include <QStandardPaths>

struct CachedConversionMetadata {
    QString packagePath;
    QString packageHash;
    qint64 packageSize = 0;
    qint64 packageLastModifiedMs = 0;
    QString appImagePath;
    QString appImageHash;
    qint64 createdAtMs = 0;

    bool isValid() const;
};

class CacheManager {
public:
    CacheManager();
    
    // Get the path where AppImages should be cached
    static QString getAppImagesDirectory();
    
    // Get the expected path for an AppImage based on package file
    // Returns: ~/AppImages/<package-name>.AppImage
    static QString getAppImagePath(const QString& packagePath);
    
    // Get the expected path for an AppImage based on package name
    static QString getAppImagePathFromPackageName(const QString& packageName);
    
    // Check if a cached AppImage exists
    static bool hasCachedAppImage(const QString& packagePath);
    
    // Check if cached AppImage is newer than package file
    // Returns true if package is newer (should rebuild), false if cache is valid
    static bool shouldRebuild(const QString& packagePath, const QString& cachedAppImagePath);
    
    // Get cached AppImage path if it exists and is valid
    // Returns empty string if no valid cache exists
    static QString getValidCachedAppImage(const QString& packagePath);

    // Persistent conversion metadata keyed by source package hash
    static QString calculatePackageHash(const QString& packagePath);
    static bool storeConversionMetadata(const QString& packagePath, const QString& appImagePath);
    static CachedConversionMetadata getConversionMetadata(const QString& packagePath);
    static QString getConversionMetadataPath(const QString& packagePath);
    
    // Sanitize package name for use in filename
    static QString sanitizePackageName(const QString& packageName);
    
    // LDD caching methods
    static QStringList getLddCache(const QString& binaryHash);
    static void setLddCache(const QString& binaryHash, const QStringList& lddOutput);
    static QString calculateBinaryHash(const QString& binaryPath);

private:
    static QString getConversionCacheDirectory();
    static QString extractPackageNameFromPath(const QString& packagePath);
};

#endif // CACHE_MANAGER_H

