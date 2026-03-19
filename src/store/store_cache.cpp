#include "store/store_cache.h"
#include <QStandardPaths>
#include <QFileInfo>

StoreCache::StoreCache(QObject* parent)
    : QObject(parent) {
    QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (base.isEmpty()) {
        base = QDir::homePath() + "/.cache/appalchemist";
    }
    m_cacheDir = QDir(base + "/store/icons");
    if (!m_cacheDir.exists()) {
        m_cacheDir.mkpath(".");
    }
}

QPixmap StoreCache::getIcon(const QString& key, int size) {
    QString cacheKey = key + ":" + QString::number(size);
    if (m_memoryCache.contains(cacheKey)) {
        return m_memoryCache.value(cacheKey);
    }
    QString path = iconCachePath(key, size);
    if (QFileInfo::exists(path)) {
        QPixmap icon(path);
        if (!icon.isNull()) {
            m_memoryCache.insert(cacheKey, icon);
            return icon;
        }
    }
    return QPixmap();
}

void StoreCache::putIcon(const QString& key, const QPixmap& icon, int size) {
    if (key.isEmpty() || icon.isNull()) return;
    QString cacheKey = key + ":" + QString::number(size);
    m_memoryCache.insert(cacheKey, icon);
    QString path = iconCachePath(key, size);
    icon.save(path, "PNG");
}

QString StoreCache::iconCachePath(const QString& key, int size) const {
    QString safe = key;
    safe.replace("/", "_");
    safe.replace(":", "_");
    return m_cacheDir.absoluteFilePath(QString("%1_%2.png").arg(safe).arg(size));
}

