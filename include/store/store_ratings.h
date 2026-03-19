#ifndef STORE_RATINGS_H
#define STORE_RATINGS_H

#include <QObject>
#include <QList>
#include <QDateTime>

struct StoreRatingEntry {
    int id = 0;
    QString appId;
    int rating = 0;
    QString comment;
    QDateTime createdAt;
};

class StoreRatings : public QObject {
    Q_OBJECT

public:
    explicit StoreRatings(QObject* parent = nullptr);

    bool initialize();
    bool addRating(const QString& appId, int rating, const QString& comment);
    QList<StoreRatingEntry> getRatings(const QString& appId);
    double getAverageRating(const QString& appId);
    int getRatingCount(const QString& appId);

private:
    QString dbPath() const;
    bool ensureSchema();
};

#endif // STORE_RATINGS_H

