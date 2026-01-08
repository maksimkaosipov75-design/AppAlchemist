#include "cache_manager.h"
#include "utils.h"
#include <QFileInfo>
#include <QDir>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>

CacheManager::CacheManager() {
}

QString CacheManager::getAppImagesDirectory() {
    QString homeDir = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    QString appImagesDir = QString("%1/AppImages").arg(homeDir);
    
    // Ensure directory exists
    QDir dir;
    if (!dir.exists(appImagesDir)) {
        dir.mkpath(appImagesDir);
    }
    
    return appImagesDir;
}

QString CacheManager::sanitizePackageName(const QString& packageName) {
    QString sanitized = packageName;
    
    // Remove or replace invalid filename characters
    sanitized.replace(QRegularExpression("[^a-zA-Z0-9._-]"), "-");
    
    // Remove multiple consecutive dashes
    sanitized.replace(QRegularExpression("-+"), "-");
    
    // Remove leading/trailing dashes
    sanitized = sanitized.trimmed();
    while (sanitized.startsWith("-")) {
        sanitized = sanitized.mid(1);
    }
    while (sanitized.endsWith("-")) {
        sanitized.chop(1);
    }
    
    // Ensure it's not empty
    if (sanitized.isEmpty()) {
        sanitized = "unknown-package";
    }
    
    return sanitized;
}

QString CacheManager::extractPackageNameFromPath(const QString& packagePath) {
    QFileInfo packageInfo(packagePath);
    QString baseName = packageInfo.baseName();
    
    // Remove common suffixes and architecture info
    // Examples:
    // - package-1.0.0-amd64.deb -> package
    // - package-1.0.0-1.x86_64.rpm -> package
    // - package_1.0.0_amd64.deb -> package
    
    // Remove .deb or .rpm extension (already done by baseName())
    
    // Try to extract package name by removing version and architecture
    // Pattern: name-version-arch or name_version_arch
    
    // First, try to remove architecture suffixes
    QRegularExpression archRegex(R"(-(amd64|x86_64|i386|i686|arm64|aarch64|noarch)(\.rpm)?$)");
    baseName.remove(archRegex);
    
    // Remove version patterns: -1.0.0, _1.0.0, -1.0.0-1, etc.
    // Try to find the last occurrence of a version-like pattern
    QRegularExpression versionRegex(R"([-_]\d+([.-]\d+)*([.-]\d+)?)");
    QRegularExpressionMatch match = versionRegex.match(baseName);
    if (match.hasMatch()) {
        int pos = match.capturedStart();
        if (pos > 0) {
            baseName = baseName.left(pos);
        }
    }
    
    // Remove any remaining dashes/underscores at the end
    baseName = baseName.trimmed();
    while (baseName.endsWith("-") || baseName.endsWith("_")) {
        baseName.chop(1);
    }
    
    return sanitizePackageName(baseName);
}

QString CacheManager::getAppImagePath(const QString& packagePath) {
    QString packageName = extractPackageNameFromPath(packagePath);
    QString appImagesDir = getAppImagesDirectory();
    return QString("%1/%2.AppImage").arg(appImagesDir).arg(packageName);
}

QString CacheManager::getAppImagePathFromPackageName(const QString& packageName) {
    QString sanitized = sanitizePackageName(packageName);
    QString appImagesDir = getAppImagesDirectory();
    return QString("%1/%2.AppImage").arg(appImagesDir).arg(sanitized);
}

bool CacheManager::hasCachedAppImage(const QString& packagePath) {
    QString appImagePath = getAppImagePath(packagePath);
    QFileInfo info(appImagePath);
    return info.exists() && info.isFile();
}

bool CacheManager::shouldRebuild(const QString& packagePath, const QString& cachedAppImagePath) {
    QFileInfo packageInfo(packagePath);
    QFileInfo appImageInfo(cachedAppImagePath);
    
    if (!packageInfo.exists() || !appImageInfo.exists()) {
        return true; // Need to rebuild if files don't exist
    }
    
    // Rebuild if package is newer than AppImage
    return packageInfo.lastModified() > appImageInfo.lastModified();
}

QString CacheManager::getValidCachedAppImage(const QString& packagePath) {
    QString cachedPath = getAppImagePath(packagePath);
    
    if (!hasCachedAppImage(packagePath)) {
        return QString(); // No cache exists
    }
    
    if (shouldRebuild(packagePath, cachedPath)) {
        return QString(); // Cache is outdated
    }
    
    return cachedPath; // Valid cache exists
}

QString CacheManager::calculateBinaryHash(const QString& binaryPath) {
    return SubprocessWrapper::generateHash(binaryPath);
}

QStringList CacheManager::getLddCache(const QString& binaryHash) {
    if (binaryHash.isEmpty()) {
        return QStringList();
    }
    
    QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QString lddCacheDir = QString("%1/appalchemist/ldd_cache").arg(cacheDir);
    QString cacheFile = QString("%1/%2.json").arg(lddCacheDir).arg(binaryHash);
    
    QFile file(cacheFile);
    if (!file.exists()) {
        return QStringList();
    }
    
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QStringList();
    }
    
    QByteArray data = file.readAll();
    file.close();
    
    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (doc.isNull() || !doc.isObject()) {
        return QStringList();
    }
    
    QJsonObject obj = doc.object();
    if (!obj.contains("ldd_output") || !obj["ldd_output"].isArray()) {
        return QStringList();
    }
    
    QJsonArray array = obj["ldd_output"].toArray();
    QStringList result;
    for (const QJsonValue& value : array) {
        result.append(value.toString());
    }
    
    return result;
}

void CacheManager::setLddCache(const QString& binaryHash, const QStringList& lddOutput) {
    if (binaryHash.isEmpty() || lddOutput.isEmpty()) {
        return;
    }
    
    QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QString lddCacheDir = QString("%1/appalchemist/ldd_cache").arg(cacheDir);
    
    QDir dir;
    if (!dir.exists(lddCacheDir)) {
        dir.mkpath(lddCacheDir);
    }
    
    QString cacheFile = QString("%1/%2.json").arg(lddCacheDir).arg(binaryHash);
    
    QJsonObject obj;
    QJsonArray array;
    for (const QString& line : lddOutput) {
        array.append(line);
    }
    obj["ldd_output"] = array;
    obj["timestamp"] = QDateTime::currentMSecsSinceEpoch();
    
    QJsonDocument doc(obj);
    
    QFile file(cacheFile);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        file.write(doc.toJson());
        file.close();
    }
}


