#ifndef STORE_VIEWMODEL_H
#define STORE_VIEWMODEL_H

#include <QObject>
#include <QTimer>
#include "store_appentry.h"
#include "store_indexer.h"
#include "store_ratings.h"
#include "store_repository.h"

class StoreViewModel : public QObject {
    Q_OBJECT

public:
    explicit StoreViewModel(StoreIndexer* indexer, StoreRatings* ratings, QObject* parent = nullptr);
    
    void setRepository(StoreRepository* repository);

    void initialize();

    void setQuery(const QString& query);
    void setCategory(const QString& category);
    void setCollection(StoreCollectionType collection);

    QList<StoreAppEntry> currentApps() const { return m_currentApps; }
    QStringList categories() const;

signals:
    void updated();

private:
    void refresh();
    void updateRatings(QList<StoreAppEntry>& apps);

    StoreIndexer* m_indexer;
    StoreRatings* m_ratings;
    StoreRepository* m_repository;
    QString m_query;
    QString m_category;
    StoreCollectionType m_collection;
    QList<StoreAppEntry> m_currentApps;
    QTimer* m_searchDebounceTimer;
    bool m_pendingAsyncSearch;
    
private slots:
    void onRepositorySearchCompleted(const QList<StoreAppEntry>& results);
    void onDebouncedSearch();
};

#endif // STORE_VIEWMODEL_H

