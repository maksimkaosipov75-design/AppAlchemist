#ifndef STORE_REPOSITORY_H
#define STORE_REPOSITORY_H

#include <QObject>
#include <QList>
#include <QPixmap>
#include "store_appentry.h"
#include "appstream_metadata.h"
#include "repository_browser.h"

class StoreRepository : public QObject {
    Q_OBJECT

public:
    explicit StoreRepository(QObject* parent = nullptr);

    QList<StoreAppEntry> loadAppStreamApps();
    QList<StoreAppEntry> searchRepositories(const QString& query, int limit = 50);
    void searchRepositoriesAsync(const QString& query, int limit = 50);

    QPixmap loadIcon(const StoreAppEntry& app, int size);

    void downloadPackage(const StoreAppEntry& app, const QString& outputDir);

    void setSudoPassword(const QString& password);

signals:
    void searchStarted();
    void searchCompleted(const QList<StoreAppEntry>& results);
    void downloadCompleted(const QString& path);
    void downloadError(const QString& message);
    void downloadProgress(qint64 bytesReceived, qint64 bytesTotal);

private slots:
    void onRepoSearchCompleted(QList<PackageInfo> results);

private:
    StoreAppEntry fromAppInfo(const AppInfo& app) const;
    StoreAppEntry fromPackageInfo(const PackageInfo& pkg) const;

    AppStreamMetadata* m_appStream;
    RepositoryBrowser* m_repoBrowser;
    int m_searchLimit = 50;
};

#endif // STORE_REPOSITORY_H

