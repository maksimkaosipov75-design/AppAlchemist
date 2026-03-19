#include "repository_search_worker.h"
#include "repository_browser.h"
#include <QDebug>

RepositorySearchWorker::RepositorySearchWorker(RepositoryBrowser* browser, QObject* parent)
    : QObject(parent)
    , m_browser(browser) {
}

void RepositorySearchWorker::performSearch(const QString& query) {
    if (!m_browser) {
        emit searchError("RepositoryBrowser is null");
        return;
    }

    // Perform search directly (this will block, but in worker thread)
    // We need to call the internal search methods directly
    PackageManager pm = RepositoryBrowser::detectPackageManager();
    QList<PackageInfo> results;
    
    switch (pm) {
        case PackageManager::APT:
            results = m_browser->searchApt(query, true); // silent mode to avoid signal issues
            break;
        case PackageManager::DNF:
            results = m_browser->searchDnf(query, true);
            break;
        case PackageManager::PACMAN:
            results = m_browser->searchPacman(query, true);
            break;
        case PackageManager::ZYPPER:
            results = m_browser->searchZypper(query, true);
            break;
        default:
            emit searchError("No supported package manager found");
            return;
    }
    
    emit searchCompleted(results);
}

