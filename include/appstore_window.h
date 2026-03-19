#ifndef APPSTORE_WINDOW_H
#define APPSTORE_WINDOW_H

#include <QDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QComboBox>
#include <QScrollArea>
#include <QGridLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QButtonGroup>
#include <QCompleter>
#include <QProgressBar>
#include <QLabel>
#include <QTimer>
#include "appstore_model.h"
#include "app_card_widget.h"
#include "app_details_dialog.h"
#include "repository_browser.h"

class AppStoreWindow : public QDialog {
    Q_OBJECT

public:
    explicit AppStoreWindow(QWidget* parent = nullptr);
    ~AppStoreWindow();
    
    // Get the selected package path after download
    QString selectedPackagePath() const { return m_downloadedPackagePath; }

signals:
    void packageSelected(const QString& packagePath);

private slots:
    void onSearchTextChanged(const QString& text);
    void onSearchClicked();
    void onCollectionChanged(int id);
    void onCategoryChanged(const QString& category);
    void onAppDownloadClicked(const AppInfo& appInfo);
    void onAppDetailsClicked(const AppInfo& appInfo);
    void onDownloadCompleted(const QString& packagePath);
    void onDownloadError(const QString& errorMessage);
    void onDownloadProgress(qint64 bytesReceived, qint64 bytesTotal);
    void onModelInitialized();
    void onModelLoadingProgress(int current, int total);
    void onRepositorySearchResults(QList<AppInfo> results);  // Pass by VALUE
    void onLoadMoreClicked();

private:
    void setupUI();
    void setupSearchBar();
    void setupCollections(QHBoxLayout* layout);
    void setupCategories();
    void setupAppGrid();
    void updateAppGrid();
    void updateButtonStates();
    void updateCategoryCombo();
    QString requestSudoPassword();
    void showLoadingIndicator(bool show);
    
    // UI Components
    QLineEdit* m_searchInput;
    QPushButton* m_searchButton;
    QCompleter* m_searchCompleter;
    QButtonGroup* m_collectionGroup;
    QPushButton* m_allButton;
    QPushButton* m_popularButton;
    QPushButton* m_newButton;
    QPushButton* m_recommendedButton;
    QComboBox* m_categoryCombo;
    QScrollArea* m_scrollArea;
    QWidget* m_scrollContent;
    QGridLayout* m_gridLayout;
    QProgressBar* m_progressBar;
    QLabel* m_statusLabel;
    QLabel* m_loadingLabel;
    QPushButton* m_loadMoreButton;
    QPushButton* m_cancelButton;
    
    // Data
    AppStoreModel* m_model;
    RepositoryBrowser* m_browser;
    QList<AppInfo> m_currentApps;
    CollectionType m_currentCollection;
    QString m_currentCategory;
    QString m_currentSearchQuery;
    QString m_downloadedPackagePath;
    
    // State
    bool m_isSearching;
    bool m_isDownloading;
    bool m_modelInitialized;
    int m_displayedAppsCount;
    static const int APPS_PER_PAGE = 20;
    
    // Timer for delayed search
    QTimer* m_searchTimer;
};

#endif // APPSTORE_WINDOW_H

