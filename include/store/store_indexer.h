#ifndef STORE_INDEXER_H
#define STORE_INDEXER_H

#include <QObject>
#include <QHash>
#include <QSet>
#include "store_appentry.h"
#include "store_repository.h"

class StoreIndexer : public QObject {
    Q_OBJECT

public:
    explicit StoreIndexer(StoreRepository* repository, QObject* parent = nullptr);

    void initialize();
    bool isInitialized() const { return m_initialized; }

    QList<StoreAppEntry> allApps() const { return m_baseApps; }
    QStringList categories() const;

    QList<StoreAppEntry> search(const QString& query) const;
    QList<StoreAppEntry> searchWithFallback(const QString& query) const;
    QList<StoreAppEntry> appsByCategory(const QString& category) const;
    QList<StoreAppEntry> byCollection(StoreCollectionType type) const;

private:
    QString normalizeToken(const QString& value) const;
    bool matchesQuery(const StoreAppEntry& app, const QString& query) const;
    bool isPopular(const StoreAppEntry& app) const;
    bool isRecommended(const StoreAppEntry& app) const;
    bool isNew(const StoreAppEntry& app) const;
    QStringList popularList() const;
    void buildCollections();
    QList<StoreAppEntry> buildPopular() const;
    QList<StoreAppEntry> buildNew() const;
    QList<StoreAppEntry> buildRecommended() const;
    QStringList keysForApp(const StoreAppEntry& app) const;

    StoreRepository* m_repository;
    bool m_initialized;
    QList<StoreAppEntry> m_baseApps;
    QHash<QString, StoreAppEntry> m_byId;
    QList<StoreAppEntry> m_popularApps;
    QList<StoreAppEntry> m_newApps;
    QList<StoreAppEntry> m_recommendedApps;
};

#endif // STORE_INDEXER_H

