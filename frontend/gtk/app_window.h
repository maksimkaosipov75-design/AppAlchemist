#ifndef APPALCHEMIST_GTK_APP_WINDOW_H
#define APPALCHEMIST_GTK_APP_WINDOW_H

#include <adwaita.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include "conversion_controller.h"
#include "repository_browser.h"

class AppWindow {
public:
    explicit AppWindow(AdwApplication* app);
    void present();

private:
    void buildUi();
    void loadCss();
    void updateFileSummary();
    void updateOutputSummary();
    void updateActions();
    void appendLog(const std::string& message);
    void setStatus(const std::string& message);
    void setProgress(double fraction);
    void updateSearchState(const std::string& message);
    void refreshSearchResults();
    void startConversion();
    void requestCancel();
    void openOutputFolder();
    void chooseFiles();
    void chooseOutputFolder();
    void startRepositorySearch();
    void downloadRepositoryPackage(int index);
    CompressionLevel selectedCompression() const;
    void requestSecret(const std::string& title,
                       const std::string& body,
                       const std::function<void(const std::string&, bool)>& handler);

    static void onSelectFilesClicked(GtkButton* button, gpointer userData);
    static void onSelectOutputClicked(GtkButton* button, gpointer userData);
    static void onConvertClicked(GtkButton* button, gpointer userData);
    static void onOpenOutputClicked(GtkButton* button, gpointer userData);
    static void onSearchRepositoryClicked(GtkButton* button, gpointer userData);
    static void onRepositoryDownloadClicked(GtkButton* button, gpointer userData);
    static gboolean onDropFiles(GtkDropTarget* target, const GValue* value, double x, double y, gpointer userData);
    static void onFilesDialogFinished(GObject* sourceObject, GAsyncResult* result, gpointer userData);
    static void onOutputDialogFinished(GObject* sourceObject, GAsyncResult* result, gpointer userData);

    AdwApplication* m_app;
    GtkWidget* m_window;
    GtkWidget* m_dropCard;
    GtkWidget* m_heroActionsBox;
    GtkWidget* m_primaryActionsBox;
    GtkWidget* m_repositorySearchRow;
    GtkWidget* m_fileSummaryLabel;
    GtkWidget* m_outputSummaryLabel;
    GtkWidget* m_statusLabel;
    GtkWidget* m_statusDetailLabel;
    GtkWidget* m_progressBar;
    GtkWidget* m_logView;
    GtkWidget* m_selectFilesButton;
    GtkWidget* m_selectOutputButton;
    GtkWidget* m_convertButton;
    GtkWidget* m_openOutputButton;
    GtkWidget* m_searchEntry;
    GtkWidget* m_searchButton;
    GtkWidget* m_searchStatusLabel;
    GtkWidget* m_searchResultsBox;
    GtkWidget* m_optimizeSwitch;
    GtkWidget* m_dependencySwitch;
    GtkWidget* m_compressionDropdown;

    std::vector<std::string> m_packagePaths;
    std::string m_outputDir;
    std::atomic<bool> m_running;
    std::mutex m_controllerMutex;
    ConversionController* m_activeController;
    RepositoryBrowser* m_repositoryBrowser;
    QList<PackageInfo> m_searchResults;
    bool m_isSearching;
    bool m_isDownloading;
    bool m_hasSearchRequest;
};

#endif // APPALCHEMIST_GTK_APP_WINDOW_H
