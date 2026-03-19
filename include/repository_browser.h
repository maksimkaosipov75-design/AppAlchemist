#ifndef REPOSITORY_BROWSER_H
#define REPOSITORY_BROWSER_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QThread>
#include <QAtomicInt>

class RepositorySearchWorker;

// Package manager type
enum class PackageManager {
    APT,        // Debian/Ubuntu
    DNF,        // Fedora/RHEL
    PACMAN,     // Arch Linux
    ZYPPER,     // openSUSE
    UNKNOWN
};

// Package information from search results
struct PackageInfo {
    QString name;
    QString version;
    QString description;
    QString architecture;
    qint64 size = 0;           // Size in bytes
    QString repository;         // Repository name (e.g., "main", "universe")
    QString downloadUrl;        // Direct download URL (if available)
    PackageManager source = PackageManager::UNKNOWN;
    
    QString sizeFormatted() const {
        if (size < 1024) return QString("%1 B").arg(size);
        if (size < 1024 * 1024) return QString("%1 KB").arg(size / 1024);
        return QString("%1 MB").arg(size / 1024.0 / 1024.0, 0, 'f', 1);
    }
};

class RepositoryBrowser : public QObject {
    Q_OBJECT

public:
    explicit RepositoryBrowser(QObject* parent = nullptr);
    ~RepositoryBrowser();
    
    // Detect available package manager
    static PackageManager detectPackageManager();
    static QString packageManagerName(PackageManager pm);
    
    // Search packages by name (synchronous)
    void searchPackages(const QString& query);
    
    // Search packages asynchronously (non-blocking)
    void searchPackagesAsync(const QString& query);
    
    // Download package to specified directory
    void downloadPackage(const PackageInfo& package, const QString& outputDir);
    
    // Set sudo password for operations that require it
    void setSudoPassword(const QString& password) { m_sudoPassword = password; }
    
    // Cancel current operation
    void cancel();
    
    // Get last search results
    QList<PackageInfo> searchResults() const { return m_searchResults; }

signals:
    void searchStarted();
    void searchProgress(int current, int total);
    void searchCompleted(QList<PackageInfo> results);  // Pass by VALUE to avoid dangling references
    void searchError(const QString& errorMessage);
    
    void downloadStarted(const QString& packageName);
    void downloadProgress(qint64 bytesReceived, qint64 bytesTotal);
    void downloadCompleted(const QString& packagePath);
    void downloadError(const QString& errorMessage);
    
    void log(const QString& message);

private slots:
    void onDownloadProgress(qint64 bytesReceived, qint64 bytesTotal);
    void onDownloadFinished();
    void onDownloadError(QNetworkReply::NetworkError error);

private slots:
    void onSearchThreadFinished();

private:
    QList<PackageInfo> m_searchResults;
    QNetworkAccessManager* m_networkManager;
    QNetworkReply* m_currentDownload;
    QString m_downloadOutputDir;
    QString m_downloadPackageName;
    QString m_sudoPassword;
    QAtomicInt m_cancelled; // Thread-safe cancellation flag
    QThread* m_searchThread;
    RepositorySearchWorker* m_searchWorker;
    
    // Search implementations for different package managers
    // Made public for worker thread access
    QList<PackageInfo> searchApt(const QString& query, bool silent = false);
    QList<PackageInfo> searchDnf(const QString& query, bool silent = false);
    QList<PackageInfo> searchPacman(const QString& query, bool silent = false);
    QList<PackageInfo> searchZypper(const QString& query, bool silent = false);
    
    // Friend class for worker access
    friend class RepositorySearchWorker;
    
    // Download implementations
    bool downloadApt(const PackageInfo& package, const QString& outputDir);
    bool downloadDnf(const PackageInfo& package, const QString& outputDir);
    bool downloadPacman(const PackageInfo& package, const QString& outputDir);
    
    // Parse package info from different formats
    PackageInfo parseAptShowOutput(const QString& output);
    PackageInfo parseDnfInfoOutput(const QString& output);
};

#endif // REPOSITORY_BROWSER_H

