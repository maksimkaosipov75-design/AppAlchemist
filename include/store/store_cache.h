#ifndef STORE_CACHE_H
#define STORE_CACHE_H

#include <QObject>
#include <QHash>
#include <QPixmap>
#include <QDir>

class StoreCache : public QObject {
    Q_OBJECT

public:
    explicit StoreCache(QObject* parent = nullptr);

    QPixmap getIcon(const QString& key, int size);
    void putIcon(const QString& key, const QPixmap& icon, int size);

private:
    QString iconCachePath(const QString& key, int size) const;

    QHash<QString, QPixmap> m_memoryCache;
    QDir m_cacheDir;
};

#endif // STORE_CACHE_H

