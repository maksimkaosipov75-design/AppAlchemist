#include "store/store_ratings.h"
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QStandardPaths>
#include <QDir>

StoreRatings::StoreRatings(QObject* parent)
    : QObject(parent) {
    initialize();
}

bool StoreRatings::initialize() {
    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "store_ratings");
    db.setDatabaseName(dbPath());
    if (!db.open()) {
        return false;
    }
    return ensureSchema();
}

bool StoreRatings::ensureSchema() {
    QSqlDatabase db = QSqlDatabase::database("store_ratings");
    QSqlQuery query(db);
    return query.exec(
        "CREATE TABLE IF NOT EXISTS ratings ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "app_id TEXT NOT NULL,"
        "rating INTEGER NOT NULL,"
        "comment TEXT,"
        "created_at INTEGER NOT NULL"
        ")"
    );
}

bool StoreRatings::addRating(const QString& appId, int rating, const QString& comment) {
    if (appId.isEmpty() || rating < 1 || rating > 5) return false;
    QSqlDatabase db = QSqlDatabase::database("store_ratings");
    if (!db.isOpen()) return false;
    QSqlQuery query(db);
    query.prepare("INSERT INTO ratings (app_id, rating, comment, created_at) VALUES (?, ?, ?, ?)");
    query.addBindValue(appId);
    query.addBindValue(rating);
    query.addBindValue(comment);
    query.addBindValue(QDateTime::currentSecsSinceEpoch());
    return query.exec();
}

QList<StoreRatingEntry> StoreRatings::getRatings(const QString& appId) {
    QList<StoreRatingEntry> results;
    QSqlDatabase db = QSqlDatabase::database("store_ratings");
    if (!db.isOpen()) return results;
    QSqlQuery query(db);
    query.prepare("SELECT id, rating, comment, created_at FROM ratings WHERE app_id = ? ORDER BY created_at DESC");
    query.addBindValue(appId);
    if (!query.exec()) return results;
    while (query.next()) {
        StoreRatingEntry entry;
        entry.id = query.value(0).toInt();
        entry.appId = appId;
        entry.rating = query.value(1).toInt();
        entry.comment = query.value(2).toString();
        entry.createdAt = QDateTime::fromSecsSinceEpoch(query.value(3).toLongLong());
        results.append(entry);
    }
    return results;
}

double StoreRatings::getAverageRating(const QString& appId) {
    QSqlDatabase db = QSqlDatabase::database("store_ratings");
    if (!db.isOpen()) return 0.0;
    QSqlQuery query(db);
    query.prepare("SELECT AVG(rating) FROM ratings WHERE app_id = ?");
    query.addBindValue(appId);
    if (!query.exec() || !query.next()) return 0.0;
    return query.value(0).toDouble();
}

int StoreRatings::getRatingCount(const QString& appId) {
    QSqlDatabase db = QSqlDatabase::database("store_ratings");
    if (!db.isOpen()) return 0;
    QSqlQuery query(db);
    query.prepare("SELECT COUNT(*) FROM ratings WHERE app_id = ?");
    query.addBindValue(appId);
    if (!query.exec() || !query.next()) return 0;
    return query.value(0).toInt();
}

QString StoreRatings::dbPath() const {
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty()) {
        base = QDir::homePath() + "/.local/share/appalchemist";
    }
    QDir dir(base);
    if (!dir.exists()) {
        dir.mkpath(".");
    }
    return dir.absoluteFilePath("store_ratings.sqlite");
}

