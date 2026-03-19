#ifndef APPSTORE_MODEL_H
#define APPSTORE_MODEL_H

#include <QObject>
#include <QList>
#include <QString>
#include <QStringList>
#include "appstream_metadata.h"
#include "repository_browser.h"

enum class CollectionType {
    All,
    Popular,
    New,
    Recommended
};

class AppStoreModel : public QObject {
    Q_OBJECT

public:
    explicit AppStoreModel(QObject* parent = nullptr);
    ~AppStoreModel();
    
    // Initialize model - load all apps
    void initialize();
    
    // Load popular packages from repositories
    void loadPopularPackagesFromRepositories();
    
    // Get apps by collection type
    QList<AppInfo> getApps(CollectionType type = CollectionType::All);
    
    // Get apps by category
    QList<AppInfo> getAppsByCategory(const QString& category);
    
    // Search apps (searches in AppStream first, then repositories if needed)
    QList<AppInfo> searchApps(const QString& query);
    
    // Search apps in repositories (async)
    void searchAppsInRepositories(const QString& query);
    
    // Get popular apps
    QList<AppInfo> getPopularApps(int limit = 50);
    
    // Get new apps
    QList<AppInfo> getNewApps(int limit = 50);
    
    // Get recommended apps
    QList<AppInfo> getRecommendedApps(int limit = 50);
    
    // Get app by name
    AppInfo getApp(const QString& name);
    
    // Get all categories with app counts
    QMap<QString, int> getCategoriesWithCounts();
    
    // Get all apps
    QList<AppInfo> getAllApps() const { return m_allApps; }
    
    // Check if initialized
    bool isInitialized() const { return m_initialized; }
    
    // Refresh data
    void refresh();

signals:
    void initialized();
    void loadingProgress(int current, int total);
    void loadingError(const QString& error);
    void repositorySearchCompleted(QList<AppInfo> results);  // Pass by VALUE to avoid dangling references

private slots:
    void onRepositorySearchCompleted(QList<PackageInfo> results);  // Pass by VALUE
    void onRepositorySearchFinished();
    void onAppStreamLoadingFinished();
    void onAppStreamLoadingProgress(int current, int total);

private:
    // Merge AppStream data with repository data
    void mergeAppData();
    
    // Determine if app is popular
    bool isAppPopular(const AppInfo& app);
    
    // Determine if app is new
    bool isAppNew(const AppInfo& app);
    
    // Determine if app is recommended
    bool isAppRecommended(const AppInfo& app);
    
    // Calculate app score for ranking
    int calculateAppScore(const AppInfo& app);
    
    // Sort apps by score
    void sortAppsByScore(QList<AppInfo>& apps);
    
    QList<AppInfo> m_allApps;
    QList<AppInfo> m_popularApps;
    QList<AppInfo> m_newApps;
    QList<AppInfo> m_recommendedApps;
    
    AppStreamMetadata* m_appStream;
    RepositoryBrowser* m_repositoryBrowser;
    
    bool m_initialized;
    bool m_appStreamLoaded;
    bool m_repositoryLoaded;
    
    // Cache for categories
    QMap<QString, int> m_categoryCounts;
    
    // Popular apps list for quick lookup
    static QStringList getPopularAppsList();
    
    // Current repository search query
    QString m_currentRepositorySearchQuery;
    QList<PackageInfo> m_pendingRepositoryResults;
};

#endif // APPSTORE_MODEL_H

