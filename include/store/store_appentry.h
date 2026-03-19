#ifndef STORE_APPENTRY_H
#define STORE_APPENTRY_H

#include <QString>
#include <QStringList>
#include <QPixmap>
#include <QDateTime>
#include "repository_browser.h"

enum class StoreCollectionType {
    All,
    Popular,
    New,
    Recommended
};

struct StoreAppEntry {
    QString appId;
    QString desktopId;
    QString packageName;
    QString displayName;
    QString summary;
    QString description;
    QStringList categories;
    QString iconKey;
    QString iconPath;
    QString version;
    qint64 size = 0;
    PackageManager source = PackageManager::UNKNOWN;
    QString repository;
    QDateTime releaseDate;

    // Ratings (local)
    double ratingAverage = 0.0;
    int ratingCount = 0;

    // Default constructor
    StoreAppEntry() = default;

    // Deep copy constructor to avoid QString corruption
    StoreAppEntry(const StoreAppEntry& other)
        : appId(QString::fromUtf8(other.appId.toUtf8()))
        , desktopId(QString::fromUtf8(other.desktopId.toUtf8()))
        , packageName(QString::fromUtf8(other.packageName.toUtf8()))
        , displayName(QString::fromUtf8(other.displayName.toUtf8()))
        , summary(QString::fromUtf8(other.summary.toUtf8()))
        , description(QString::fromUtf8(other.description.toUtf8()))
        , categories(other.categories)
        , iconKey(QString::fromUtf8(other.iconKey.toUtf8()))
        , iconPath(QString::fromUtf8(other.iconPath.toUtf8()))
        , version(QString::fromUtf8(other.version.toUtf8()))
        , size(other.size)
        , source(other.source)
        , repository(QString::fromUtf8(other.repository.toUtf8()))
        , releaseDate(other.releaseDate)
        , ratingAverage(other.ratingAverage)
        , ratingCount(other.ratingCount)
    {}

    // Deep copy assignment operator
    StoreAppEntry& operator=(const StoreAppEntry& other) {
        if (this != &other) {
            appId = QString::fromUtf8(other.appId.toUtf8());
            desktopId = QString::fromUtf8(other.desktopId.toUtf8());
            packageName = QString::fromUtf8(other.packageName.toUtf8());
            displayName = QString::fromUtf8(other.displayName.toUtf8());
            summary = QString::fromUtf8(other.summary.toUtf8());
            description = QString::fromUtf8(other.description.toUtf8());
            categories = other.categories;
            iconKey = QString::fromUtf8(other.iconKey.toUtf8());
            iconPath = QString::fromUtf8(other.iconPath.toUtf8());
            version = QString::fromUtf8(other.version.toUtf8());
            size = other.size;
            source = other.source;
            repository = QString::fromUtf8(other.repository.toUtf8());
            releaseDate = other.releaseDate;
            ratingAverage = other.ratingAverage;
            ratingCount = other.ratingCount;
        }
        return *this;
    }

    // Derived helpers - return deep copy to avoid QString corruption
    QString effectiveName() const {
        QString result = displayName.isEmpty() ? packageName : displayName;
        // Force deep copy
        return QString::fromUtf8(result.toUtf8());
    }

    QString sizeFormatted() const {
        if (size < 1024) return QString("%1 B").arg(size);
        if (size < 1024 * 1024) return QString("%1 KB").arg(size / 1024);
        return QString("%1 MB").arg(size / 1024.0 / 1024.0, 0, 'f', 1);
    }
};

#endif // STORE_APPENTRY_H

