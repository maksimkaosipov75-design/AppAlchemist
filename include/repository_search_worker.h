#ifndef REPOSITORY_SEARCH_WORKER_H
#define REPOSITORY_SEARCH_WORKER_H

#include <QObject>
#include <QThread>
#include "repository_browser.h"

class RepositorySearchWorker : public QObject {
    Q_OBJECT

public:
    explicit RepositorySearchWorker(RepositoryBrowser* browser, QObject* parent = nullptr);

public slots:
    void performSearch(const QString& query);

signals:
    void searchRequested(const QString& query);

signals:
    void searchCompleted(QList<PackageInfo> results);
    void searchError(const QString& error);

private:
    RepositoryBrowser* m_browser;
};

#endif // REPOSITORY_SEARCH_WORKER_H

