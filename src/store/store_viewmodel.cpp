#include "store/store_viewmodel.h"

StoreViewModel::StoreViewModel(StoreIndexer* indexer, StoreRatings* ratings, QObject* parent)
    : QObject(parent)
    , m_indexer(indexer)
    , m_ratings(ratings)
    , m_repository(nullptr)
    , m_collection(StoreCollectionType::All)
    , m_searchDebounceTimer(new QTimer(this))
    , m_pendingAsyncSearch(false) {
    m_searchDebounceTimer->setSingleShot(true);
    m_searchDebounceTimer->setInterval(400); // 400ms debounce
    connect(m_searchDebounceTimer, &QTimer::timeout, this, &StoreViewModel::onDebouncedSearch);
}

void StoreViewModel::setRepository(StoreRepository* repository) {
    m_repository = repository;
    if (m_repository) {
        connect(m_repository, &StoreRepository::searchCompleted, this, &StoreViewModel::onRepositorySearchCompleted);
    }
}

void StoreViewModel::initialize() {
    if (!m_indexer->isInitialized()) {
        m_indexer->initialize();
    }
    refresh();
}

void StoreViewModel::setQuery(const QString& query) {
    m_query = query;
    // Use debounce for search queries
    if (!m_query.isEmpty()) {
        m_searchDebounceTimer->stop();
        m_searchDebounceTimer->start();
    } else {
        // Empty query - refresh immediately
        refresh();
    }
}

void StoreViewModel::setCategory(const QString& category) {
    m_category = category;
    m_pendingAsyncSearch = false;
    m_searchDebounceTimer->stop();
    refresh();
}

void StoreViewModel::setCollection(StoreCollectionType collection) {
    m_collection = collection;
    m_pendingAsyncSearch = false;
    m_searchDebounceTimer->stop();
    refresh();
}

QStringList StoreViewModel::categories() const {
    return m_indexer->categories();
}

void StoreViewModel::refresh() {
    QList<StoreAppEntry> base;
    if (!m_query.isEmpty()) {
        // First try local search
        base = m_indexer->search(m_query);
        if (base.isEmpty() && m_repository) {
            // No local results - trigger async repository search
            m_pendingAsyncSearch = true;
            m_repository->searchRepositoriesAsync(m_query, 50);
            // Show loading state - don't emit updated yet
            return;
        }
    } else if (!m_category.isEmpty()) {
        base = m_indexer->appsByCategory(m_category);
    } else {
        base = m_indexer->byCollection(m_collection);
    }
    updateRatings(base);
    m_currentApps = base;
    emit updated();
}

void StoreViewModel::onDebouncedSearch() {
    refresh();
}

void StoreViewModel::onRepositorySearchCompleted(const QList<StoreAppEntry>& results) {
    if (m_pendingAsyncSearch) {
        m_pendingAsyncSearch = false;
        updateRatings(const_cast<QList<StoreAppEntry>&>(results));
        m_currentApps = results;
        emit updated();
    }
}

void StoreViewModel::updateRatings(QList<StoreAppEntry>& apps) {
    for (StoreAppEntry& app : apps) {
        const QString key = !app.appId.isEmpty() ? app.appId : app.packageName;
        app.ratingAverage = m_ratings->getAverageRating(key);
        app.ratingCount = m_ratings->getRatingCount(key);
    }
}

