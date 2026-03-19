#include "app_window.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QMetaObject>
#include <gio/gio.h>
#include <glib.h>
#include <algorithm>
#include <filesystem>
#include <functional>
#include <thread>

namespace {
struct SecretDialogState {
    std::function<void(const std::string&, bool)> handler;
    GtkEditable* entry;
};

void runOnMain(std::function<void()> fn) {
    auto* work = new std::function<void()>(std::move(fn));
    g_idle_add_full(
        G_PRIORITY_DEFAULT,
        [](gpointer data) -> gboolean {
            auto* callback = static_cast<std::function<void()>*>(data);
            (*callback)();
            delete callback;
            return G_SOURCE_REMOVE;
        },
        work,
        nullptr
    );
}

std::string toStdString(const QString& value) {
    return value.toUtf8().constData();
}

std::string toStdString(const char* value) {
    return value ? value : "";
}

std::string filePathFromGFile(GFile* file) {
    char* path = g_file_get_path(file);
    std::string result = path ? path : "";
    g_free(path);
    return result;
}

QStringList toQStringList(const std::vector<std::string>& values) {
    QStringList list;
    for (const auto& value : values) {
        list.append(QString::fromUtf8(value.c_str()));
    }
    return list;
}

std::string defaultOutputDir() {
    return (std::filesystem::path(g_get_home_dir()) / "AppImages").string();
}
} // namespace

AppWindow::AppWindow(AdwApplication* app)
    : m_app(app)
    , m_window(nullptr)
    , m_dropCard(nullptr)
    , m_heroActionsBox(nullptr)
    , m_primaryActionsBox(nullptr)
    , m_repositorySearchRow(nullptr)
    , m_fileSummaryLabel(nullptr)
    , m_outputSummaryLabel(nullptr)
    , m_statusLabel(nullptr)
    , m_statusDetailLabel(nullptr)
    , m_progressBar(nullptr)
    , m_logView(nullptr)
    , m_selectFilesButton(nullptr)
    , m_selectOutputButton(nullptr)
    , m_convertButton(nullptr)
    , m_openOutputButton(nullptr)
    , m_searchEntry(nullptr)
    , m_searchButton(nullptr)
    , m_searchStatusLabel(nullptr)
    , m_searchResultsBox(nullptr)
    , m_logToggleButton(nullptr)
    , m_logRevealer(nullptr)
    , m_optimizeSwitch(nullptr)
    , m_dependencySwitch(nullptr)
    , m_compressionDropdown(nullptr)
    , m_outputDir(defaultOutputDir())
    , m_running(false)
    , m_activeController(nullptr)
    , m_repositoryBrowser(new RepositoryBrowser())
    , m_isSearching(false)
    , m_isDownloading(false)
    , m_hasSearchRequest(false)
    , m_logVisible(false)
{
    buildUi();
    loadCss();
    updateFileSummary();
    updateOutputSummary();
    refreshSearchResults();
    updateSearchState("Search your repositories when you want to reuse an already packaged system build.");
    appendLog("GTK frontend ready.");
    appendLog("Choose a package or search repositories to begin.");
    updateActions();

    QObject::connect(m_repositoryBrowser, &RepositoryBrowser::searchCompleted,
                     [this](const QList<PackageInfo>& results) {
        m_searchResults = results;
        m_isSearching = false;
        runOnMain([this]() {
            updateSearchState("Search complete.");
            refreshSearchResults();
            updateActions();
        });
    });

    QObject::connect(m_repositoryBrowser, &RepositoryBrowser::searchError,
                     [this](const QString& error) {
        m_isSearching = false;
        const auto text = toStdString(error);
        runOnMain([this, text]() {
            updateSearchState(text);
            appendLog("Repository search error: " + text);
            updateActions();
        });
    });

    QObject::connect(m_repositoryBrowser, &RepositoryBrowser::downloadCompleted,
                     [this](const QString& packagePath) {
        m_isDownloading = false;
        const auto path = toStdString(packagePath);
        runOnMain([this, path]() {
            m_packagePaths = {path};
            updateFileSummary();
            updateActions();
            updateSearchState("Package downloaded.");
            appendLog("Downloaded package: " + path);
        });
    });

    QObject::connect(m_repositoryBrowser, &RepositoryBrowser::downloadError,
                     [this](const QString& error) {
        m_isDownloading = false;
        const auto text = toStdString(error);
        runOnMain([this, text]() {
            updateSearchState(text);
            appendLog("Repository download error: " + text);
            updateActions();
        });
    });

    QObject::connect(m_repositoryBrowser, &RepositoryBrowser::downloadStarted,
                     [this](const QString& packageName) {
        m_isDownloading = true;
        const auto text = toStdString(packageName);
        runOnMain([this, text]() {
            updateSearchState("Downloading " + text + "...");
            updateActions();
        });
    });
}

void AppWindow::present() {
    gtk_window_present(GTK_WINDOW(m_window));
}

void AppWindow::buildUi() {
    m_window = adw_application_window_new(GTK_APPLICATION(m_app));
    gtk_window_set_title(GTK_WINDOW(m_window), "AppAlchemist");
    gtk_window_set_default_size(GTK_WINDOW(m_window), 1180, 820);

    auto* toolbarView = adw_toolbar_view_new();
    auto* headerBar = adw_header_bar_new();
    auto* titleWidget = adw_window_title_new("AppAlchemist", "Fast package conversion");
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(headerBar), titleWidget);
    auto* headerLogButton = gtk_button_new_from_icon_name("sidebar-show-right-symbolic");
    gtk_widget_set_tooltip_text(headerLogButton, "Toggle activity log");
    gtk_widget_add_css_class(headerLogButton, "flat");
    adw_header_bar_pack_end(ADW_HEADER_BAR(headerBar), headerLogButton);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbarView), headerBar);

    auto* scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbarView), scroller);

    auto* clamp = adw_clamp_new();
    adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 1200);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), clamp);

    auto* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_add_css_class(root, "window-shell");
    gtk_widget_set_margin_top(root, 18);
    gtk_widget_set_margin_bottom(root, 18);
    gtk_widget_set_margin_start(root, 18);
    gtk_widget_set_margin_end(root, 18);
    adw_clamp_set_child(ADW_CLAMP(clamp), root);

    auto* introBlock = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_add_css_class(introBlock, "intro-block");
    auto* eyebrow = gtk_label_new("Package workflow");
    gtk_widget_add_css_class(eyebrow, "eyebrow");
    gtk_label_set_xalign(GTK_LABEL(eyebrow), 0.0f);
    auto* pageTitle = gtk_label_new("Convert packages with less interface overhead");
    gtk_widget_add_css_class(pageTitle, "page-title");
    gtk_label_set_xalign(GTK_LABEL(pageTitle), 0.0f);
    auto* pageSubtitle = gtk_label_new(
        "Choose an input package, review only the settings that matter, then convert. Repository search and logs stay available without crowding the main path."
    );
    gtk_widget_add_css_class(pageSubtitle, "page-subtitle");
    gtk_label_set_xalign(GTK_LABEL(pageSubtitle), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(pageSubtitle), true);
    gtk_box_append(GTK_BOX(introBlock), eyebrow);
    gtk_box_append(GTK_BOX(introBlock), pageTitle);
    gtk_box_append(GTK_BOX(introBlock), pageSubtitle);
    gtk_box_append(GTK_BOX(root), introBlock);

    auto* columns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    gtk_widget_add_css_class(columns, "content-columns");
    gtk_box_append(GTK_BOX(root), columns);

    auto* mainColumn = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_add_css_class(mainColumn, "workspace-column");
    gtk_widget_set_hexpand(mainColumn, true);
    gtk_box_append(GTK_BOX(columns), mainColumn);

    auto* sideColumn = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_add_css_class(sideColumn, "sidebar-column");
    gtk_box_append(GTK_BOX(columns), sideColumn);

    m_dropCard = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_add_css_class(m_dropCard, "surface-card");
    gtk_widget_add_css_class(m_dropCard, "source-card");
    auto* dropIcon = gtk_image_new_from_icon_name("package-x-generic-symbolic");
    gtk_widget_set_size_request(dropIcon, 40, 40);
    gtk_widget_add_css_class(dropIcon, "hero-icon");
    auto* dropTitle = gtk_label_new("Source packages");
    gtk_widget_add_css_class(dropTitle, "hero-title");
    gtk_label_set_xalign(GTK_LABEL(dropTitle), 0.0f);
    auto* dropSubtitle = gtk_label_new("Drag local packages here or choose them from disk. This remains the fastest path to conversion.");
    gtk_widget_add_css_class(dropSubtitle, "hero-subtitle");
    gtk_label_set_xalign(GTK_LABEL(dropSubtitle), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(dropSubtitle), true);
    m_fileSummaryLabel = gtk_label_new("");
    gtk_widget_add_css_class(m_fileSummaryLabel, "summary-value");
    gtk_label_set_xalign(GTK_LABEL(m_fileSummaryLabel), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(m_fileSummaryLabel), true);
    auto* formatHint = gtk_label_new("Supported: .deb, .rpm, .tar.*, .zip");
    gtk_widget_add_css_class(formatHint, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(formatHint), 0.0f);
    auto* heroActions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    m_heroActionsBox = heroActions;
    gtk_widget_add_css_class(heroActions, "action-row");
    m_selectFilesButton = gtk_button_new_with_label("Choose Packages");
    gtk_widget_add_css_class(m_selectFilesButton, "suggested-action");
    m_searchButton = gtk_button_new_with_label("Search Repository");
    auto* clearButton = gtk_button_new_with_label("Clear Selection");
    gtk_widget_add_css_class(clearButton, "flat-button");
    gtk_box_append(GTK_BOX(heroActions), m_selectFilesButton);
    gtk_box_append(GTK_BOX(heroActions), m_searchButton);
    gtk_box_append(GTK_BOX(heroActions), clearButton);
    gtk_box_append(GTK_BOX(m_dropCard), dropIcon);
    gtk_box_append(GTK_BOX(m_dropCard), dropTitle);
    gtk_box_append(GTK_BOX(m_dropCard), dropSubtitle);
    gtk_box_append(GTK_BOX(m_dropCard), m_fileSummaryLabel);
    gtk_box_append(GTK_BOX(m_dropCard), formatHint);
    gtk_box_append(GTK_BOX(m_dropCard), heroActions);

    auto* target = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
    g_signal_connect(target, "drop", G_CALLBACK(AppWindow::onDropFiles), this);
    gtk_widget_add_controller(m_dropCard, GTK_EVENT_CONTROLLER(target));

    auto* statusCard = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_add_css_class(statusCard, "surface-card");
    gtk_widget_add_css_class(statusCard, "session-card");
    auto* statusHeader = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    auto* statusEyebrow = gtk_label_new("Session");
    gtk_widget_add_css_class(statusEyebrow, "eyebrow");
    gtk_label_set_xalign(GTK_LABEL(statusEyebrow), 0.0f);
    m_statusLabel = gtk_label_new("Ready");
    gtk_label_set_xalign(GTK_LABEL(m_statusLabel), 0.0f);
    gtk_widget_add_css_class(m_statusLabel, "status-label");
    m_statusDetailLabel = gtk_label_new("Choose one or more packages to unlock build actions.");
    gtk_label_set_xalign(GTK_LABEL(m_statusDetailLabel), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(m_statusDetailLabel), true);
    gtk_widget_add_css_class(m_statusDetailLabel, "status-detail");
    gtk_box_append(GTK_BOX(statusHeader), statusEyebrow);
    gtk_box_append(GTK_BOX(statusHeader), m_statusLabel);
    gtk_box_append(GTK_BOX(statusHeader), m_statusDetailLabel);

    m_progressBar = gtk_progress_bar_new();
    gtk_widget_add_css_class(m_progressBar, "status-meter");
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(m_progressBar), false);

    auto* actionRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    m_primaryActionsBox = actionRow;
    gtk_widget_add_css_class(actionRow, "action-row");
    m_convertButton = gtk_button_new_with_label("Convert");
    gtk_widget_add_css_class(m_convertButton, "suggested-action");
    m_openOutputButton = gtk_button_new_with_label("Open Folder");
    gtk_widget_add_css_class(m_openOutputButton, "flat-button");
    gtk_box_append(GTK_BOX(actionRow), m_convertButton);
    gtk_box_append(GTK_BOX(actionRow), m_openOutputButton);

    gtk_box_append(GTK_BOX(statusCard), statusHeader);
    gtk_box_append(GTK_BOX(statusCard), m_progressBar);
    gtk_box_append(GTK_BOX(statusCard), actionRow);
    gtk_box_append(GTK_BOX(sideColumn), statusCard);

    auto* settingsCard = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_add_css_class(settingsCard, "surface-card");
    gtk_widget_add_css_class(settingsCard, "settings-card");
    auto* settingsTitle = gtk_label_new("Settings");
    gtk_widget_add_css_class(settingsTitle, "section-title");
    gtk_label_set_xalign(GTK_LABEL(settingsTitle), 0.0f);
    auto* settingsSubtitle = gtk_label_new("Keep the defaults lean. Change only what matters for this build.");
    gtk_widget_add_css_class(settingsSubtitle, "panel-subtitle");
    gtk_label_set_xalign(GTK_LABEL(settingsSubtitle), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(settingsSubtitle), true);
    auto* settingsGroup = adw_preferences_group_new();
    gtk_widget_add_css_class(settingsGroup, "embedded-preferences");

    auto* outputRow = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(outputRow), "Output folder");
    m_outputSummaryLabel = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(m_outputSummaryLabel), 1.0f);
    gtk_widget_add_css_class(m_outputSummaryLabel, "dim-label");
    m_selectOutputButton = gtk_button_new_with_label("Choose");
    adw_action_row_add_suffix(ADW_ACTION_ROW(outputRow), m_outputSummaryLabel);
    adw_action_row_add_suffix(ADW_ACTION_ROW(outputRow), m_selectOutputButton);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(settingsGroup), outputRow);

    auto* optimizeRow = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(optimizeRow), "Optimize size");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(optimizeRow), "Strip and compress aggressively after packaging.");
    m_optimizeSwitch = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(m_optimizeSwitch), true);
    adw_action_row_add_suffix(ADW_ACTION_ROW(optimizeRow), m_optimizeSwitch);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(settingsGroup), optimizeRow);

    auto* depsRow = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(depsRow), "Resolve dependencies");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(depsRow), "Request missing libraries during packaging when needed.");
    m_dependencySwitch = gtk_switch_new();
    adw_action_row_add_suffix(ADW_ACTION_ROW(depsRow), m_dependencySwitch);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(settingsGroup), depsRow);

    auto* compressionRow = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(compressionRow), "Compression");
    auto* compressionModel = gtk_string_list_new(nullptr);
    gtk_string_list_append(compressionModel, "Fast");
    gtk_string_list_append(compressionModel, "Normal");
    gtk_string_list_append(compressionModel, "Maximum");
    gtk_string_list_append(compressionModel, "Ultra");
    m_compressionDropdown = gtk_drop_down_new(G_LIST_MODEL(compressionModel), nullptr);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(m_compressionDropdown), 1);
    adw_action_row_add_suffix(ADW_ACTION_ROW(compressionRow), m_compressionDropdown);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(settingsGroup), compressionRow);

    gtk_box_append(GTK_BOX(settingsCard), settingsTitle);
    gtk_box_append(GTK_BOX(settingsCard), settingsSubtitle);
    gtk_box_append(GTK_BOX(settingsCard), settingsGroup);
    gtk_box_append(GTK_BOX(sideColumn), settingsCard);

    auto* repositoryCard = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_add_css_class(repositoryCard, "surface-card");
    gtk_widget_add_css_class(repositoryCard, "repo-card");
    auto* repositoryTitle = gtk_label_new("Repository");
    gtk_label_set_xalign(GTK_LABEL(repositoryTitle), 0.0f);
    gtk_widget_add_css_class(repositoryTitle, "section-title");
    auto* repositorySubtitle = gtk_label_new("Use this only when the package is not already local.");
    gtk_label_set_xalign(GTK_LABEL(repositorySubtitle), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(repositorySubtitle), true);
    gtk_widget_add_css_class(repositorySubtitle, "panel-subtitle");
    auto* repositorySearchRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    m_repositorySearchRow = repositorySearchRow;
    gtk_widget_add_css_class(repositorySearchRow, "search-row");
    m_searchEntry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(m_searchEntry), "Search package name");
    gtk_widget_set_hexpand(m_searchEntry, true);
    auto* repositorySearchButton = gtk_button_new_with_label("Search");
    gtk_widget_add_css_class(repositorySearchButton, "flat-button");
    auto* repositoryScroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(repositoryScroll), 176);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(repositoryScroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_add_css_class(repositoryScroll, "card-scroll");
    m_searchResultsBox = gtk_list_box_new();
    gtk_widget_add_css_class(m_searchResultsBox, "results-list");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(m_searchResultsBox), GTK_SELECTION_NONE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(repositoryScroll), m_searchResultsBox);
    m_searchStatusLabel = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(m_searchStatusLabel), 0.0f);
    gtk_widget_add_css_class(m_searchStatusLabel, "dim-label");
    gtk_box_append(GTK_BOX(repositorySearchRow), m_searchEntry);
    gtk_box_append(GTK_BOX(repositorySearchRow), repositorySearchButton);
    gtk_box_append(GTK_BOX(repositoryCard), repositoryTitle);
    gtk_box_append(GTK_BOX(repositoryCard), repositorySubtitle);
    gtk_box_append(GTK_BOX(repositoryCard), repositorySearchRow);
    gtk_box_append(GTK_BOX(repositoryCard), m_searchStatusLabel);
    gtk_box_append(GTK_BOX(repositoryCard), repositoryScroll);
    gtk_box_append(GTK_BOX(mainColumn), repositoryCard);

    auto* logCard = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_add_css_class(logCard, "surface-card");
    gtk_widget_add_css_class(logCard, "log-card");
    auto* logHeader = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    auto* logHeaderText = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    auto* logTitle = gtk_label_new("Activity");
    gtk_label_set_xalign(GTK_LABEL(logTitle), 0.0f);
    gtk_widget_add_css_class(logTitle, "section-title");
    auto* logSubtitle = gtk_label_new("Keep this collapsed unless you need pipeline detail.");
    gtk_label_set_xalign(GTK_LABEL(logSubtitle), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(logSubtitle), true);
    gtk_widget_add_css_class(logSubtitle, "panel-subtitle");
    m_logToggleButton = gtk_button_new_with_label("Show Log");
    gtk_widget_add_css_class(m_logToggleButton, "flat-button");
    gtk_box_append(GTK_BOX(logHeaderText), logTitle);
    gtk_box_append(GTK_BOX(logHeaderText), logSubtitle);
    gtk_box_append(GTK_BOX(logHeader), logHeaderText);
    gtk_box_append(GTK_BOX(logHeader), m_logToggleButton);
    gtk_widget_set_hexpand(logHeaderText, true);

    auto* logScroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(logScroll), 260);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(logScroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_add_css_class(logScroll, "card-scroll");
    m_logView = gtk_text_view_new();
    gtk_widget_add_css_class(m_logView, "activity-view");
    gtk_text_view_set_editable(GTK_TEXT_VIEW(m_logView), false);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(m_logView), false);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(m_logView), true);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(logScroll), m_logView);

    m_logRevealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(m_logRevealer), GTK_REVEALER_TRANSITION_TYPE_SWING_DOWN);
    gtk_revealer_set_transition_duration(GTK_REVEALER(m_logRevealer), 180);
    gtk_revealer_set_reveal_child(GTK_REVEALER(m_logRevealer), false);
    gtk_revealer_set_child(GTK_REVEALER(m_logRevealer), logScroll);

    gtk_box_append(GTK_BOX(logCard), logHeader);
    gtk_box_append(GTK_BOX(logCard), m_logRevealer);
    gtk_box_append(GTK_BOX(mainColumn), logCard);

    auto* workflowCard = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_add_css_class(workflowCard, "surface-card");
    gtk_widget_add_css_class(workflowCard, "workflow-card");
    auto* workflowTitle = gtk_label_new("Workflow");
    gtk_widget_add_css_class(workflowTitle, "section-title");
    gtk_label_set_xalign(GTK_LABEL(workflowTitle), 0.0f);
    auto* workflowSteps = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(workflowSteps, "workflow-steps");
    auto* stepPick = gtk_label_new("1. Pick package");
    auto* stepReview = gtk_label_new("2. Review settings");
    auto* stepConvert = gtk_label_new("3. Convert");
    gtk_widget_add_css_class(stepPick, "workflow-step");
    gtk_widget_add_css_class(stepReview, "workflow-step");
    gtk_widget_add_css_class(stepConvert, "workflow-step");
    gtk_box_append(GTK_BOX(workflowCard), workflowTitle);
    gtk_box_append(GTK_BOX(workflowSteps), stepPick);
    gtk_box_append(GTK_BOX(workflowSteps), stepReview);
    gtk_box_append(GTK_BOX(workflowSteps), stepConvert);
    gtk_box_append(GTK_BOX(workflowCard), workflowSteps);
    gtk_box_prepend(GTK_BOX(mainColumn), workflowCard);
    gtk_box_prepend(GTK_BOX(mainColumn), m_dropCard);

    auto* compactActions = adw_breakpoint_new(adw_breakpoint_condition_parse("max-width: 720sp"));
    GValue vertical = G_VALUE_INIT;
    g_value_init(&vertical, GTK_TYPE_ORIENTATION);
    g_value_set_enum(&vertical, GTK_ORIENTATION_VERTICAL);
    adw_breakpoint_add_setter(compactActions, G_OBJECT(columns), "orientation", &vertical);
    adw_breakpoint_add_setter(compactActions, G_OBJECT(m_heroActionsBox), "orientation", &vertical);
    adw_breakpoint_add_setter(compactActions, G_OBJECT(m_primaryActionsBox), "orientation", &vertical);
    adw_breakpoint_add_setter(compactActions, G_OBJECT(m_repositorySearchRow), "orientation", &vertical);
    adw_breakpoint_add_setter(compactActions, G_OBJECT(workflowSteps), "orientation", &vertical);
    g_value_unset(&vertical);
    adw_application_window_add_breakpoint(ADW_APPLICATION_WINDOW(m_window), compactActions);

    adw_application_window_set_content(ADW_APPLICATION_WINDOW(m_window), toolbarView);

    g_signal_connect(m_selectFilesButton, "clicked", G_CALLBACK(AppWindow::onSelectFilesClicked), this);
    g_signal_connect(m_selectOutputButton, "clicked", G_CALLBACK(AppWindow::onSelectOutputClicked), this);
    g_signal_connect(m_convertButton, "clicked", G_CALLBACK(AppWindow::onConvertClicked), this);
    g_signal_connect(m_openOutputButton, "clicked", G_CALLBACK(AppWindow::onOpenOutputClicked), this);
    g_signal_connect(m_searchButton, "clicked", G_CALLBACK(AppWindow::onSearchRepositoryClicked), this);
    g_signal_connect(repositorySearchButton, "clicked", G_CALLBACK(AppWindow::onSearchRepositoryClicked), this);
    g_signal_connect(m_logToggleButton, "clicked", G_CALLBACK(AppWindow::onToggleLogClicked), this);
    g_signal_connect(headerLogButton, "clicked", G_CALLBACK(AppWindow::onToggleLogClicked), this);
    g_signal_connect_swapped(m_searchEntry, "activate", G_CALLBACK(+[](AppWindow* self) {
        self->startRepositorySearch();
    }), this);
    g_signal_connect_swapped(clearButton, "clicked", G_CALLBACK(+[](AppWindow* self) {
        self->m_packagePaths.clear();
        self->updateFileSummary();
        self->appendLog("Cleared package selection.");
        self->updateActions();
    }), this);
}

void AppWindow::loadCss() {
    auto* provider = gtk_css_provider_new();
    gtk_css_provider_load_from_path(provider, APPALCHEMIST_GTK_STYLE_PATH);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
    );
    g_object_unref(provider);
}

void AppWindow::updateFileSummary() {
    std::string summary;
    if (m_packagePaths.empty()) {
        summary = "No package selected yet";
    } else if (m_packagePaths.size() == 1) {
        summary = "Selected: " + std::filesystem::path(m_packagePaths.front()).filename().string();
    } else {
        summary = std::to_string(m_packagePaths.size()) + " packages selected for the next build";
    }

    gtk_label_set_text(GTK_LABEL(m_fileSummaryLabel), summary.c_str());
}

void AppWindow::updateOutputSummary() {
    gtk_label_set_text(GTK_LABEL(m_outputSummaryLabel), m_outputDir.c_str());
}

void AppWindow::updateActions() {
    const bool hasPackages = !m_packagePaths.empty();
    const bool running = m_running.load();
    const bool repoBusy = m_isSearching || m_isDownloading;

    gtk_widget_set_sensitive(m_selectFilesButton, !running);
    gtk_widget_set_sensitive(m_selectOutputButton, !running);
    gtk_widget_set_sensitive(m_openOutputButton, !running);
    gtk_widget_set_sensitive(m_optimizeSwitch, !running);
    gtk_widget_set_sensitive(m_dependencySwitch, !running);
    gtk_widget_set_sensitive(m_compressionDropdown, !running);
    gtk_widget_set_sensitive(m_searchEntry, !running && !repoBusy);
    gtk_widget_set_sensitive(m_searchButton, !running && !repoBusy);
    gtk_button_set_label(GTK_BUTTON(m_convertButton), running ? "Cancel" : "Convert");
    gtk_widget_set_sensitive(m_convertButton, running || hasPackages);

    std::string detail;
    if (running) {
        detail = "Conversion is active. Cancellation is available, but the current packaging step may take a moment to unwind.";
    } else if (repoBusy) {
        detail = "Repository activity is in progress. Your conversion settings stay available while search or download finishes.";
    } else if (hasPackages) {
        detail = "Source selection is ready. Review settings on the right, then start conversion.";
    } else {
        detail = "Choose local packages first, or search repositories if you want to stage an input package.";
    }

    gtk_label_set_text(GTK_LABEL(m_statusDetailLabel), detail.c_str());
}

void AppWindow::toggleLogVisibility() {
    m_logVisible = !m_logVisible;
    gtk_revealer_set_reveal_child(GTK_REVEALER(m_logRevealer), m_logVisible);
    gtk_button_set_label(GTK_BUTTON(m_logToggleButton), m_logVisible ? "Hide Log" : "Show Log");
}

void AppWindow::appendLog(const std::string& message) {
    if (!m_logVisible && message.rfind("ERROR:", 0) == 0) {
        toggleLogVisibility();
    }

    auto* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(m_logView));
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buffer, &end);
    const std::string line = message + "\n";
    gtk_text_buffer_insert(buffer, &end, line.c_str(), -1);
    gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(m_logView), &end, 0.0, false, 0.0, 1.0);
}

void AppWindow::setStatus(const std::string& message) {
    gtk_label_set_text(GTK_LABEL(m_statusLabel), message.c_str());
}

void AppWindow::setProgress(double fraction) {
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(m_progressBar), std::clamp(fraction, 0.0, 1.0));
}

void AppWindow::updateSearchState(const std::string& message) {
    gtk_label_set_text(GTK_LABEL(m_searchStatusLabel), message.c_str());
}

void AppWindow::refreshSearchResults() {
    GtkWidget* child = gtk_widget_get_first_child(m_searchResultsBox);
    while (child) {
        GtkWidget* next = gtk_widget_get_next_sibling(child);
        gtk_list_box_remove(GTK_LIST_BOX(m_searchResultsBox), child);
        child = next;
    }

    if (!m_hasSearchRequest && !m_isSearching) {
        auto* emptyBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_widget_set_margin_top(emptyBox, 12);
        gtk_widget_set_margin_bottom(emptyBox, 12);
        gtk_widget_set_margin_start(emptyBox, 12);
        gtk_widget_set_margin_end(emptyBox, 12);
        auto* emptyLabel = gtk_label_new("Repository results will appear here.");
        auto* emptyHint = gtk_label_new("Search by package name to pull a package directly into the conversion queue.");
        gtk_label_set_xalign(GTK_LABEL(emptyLabel), 0.0f);
        gtk_label_set_xalign(GTK_LABEL(emptyHint), 0.0f);
        gtk_widget_add_css_class(emptyLabel, "dim-label");
        gtk_widget_add_css_class(emptyHint, "support-copy");
        gtk_box_append(GTK_BOX(emptyBox), emptyLabel);
        gtk_box_append(GTK_BOX(emptyBox), emptyHint);
        gtk_list_box_append(GTK_LIST_BOX(m_searchResultsBox), emptyBox);
        return;
    }

    if (m_isSearching) {
        auto* emptyBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_widget_set_margin_top(emptyBox, 12);
        gtk_widget_set_margin_bottom(emptyBox, 12);
        gtk_widget_set_margin_start(emptyBox, 12);
        gtk_widget_set_margin_end(emptyBox, 12);
        auto* emptyLabel = gtk_label_new("Searching repositories...");
        auto* emptyHint = gtk_label_new("Results appear here as soon as the package manager responds.");
        gtk_label_set_xalign(GTK_LABEL(emptyLabel), 0.0f);
        gtk_label_set_xalign(GTK_LABEL(emptyHint), 0.0f);
        gtk_widget_add_css_class(emptyLabel, "dim-label");
        gtk_widget_add_css_class(emptyHint, "support-copy");
        gtk_box_append(GTK_BOX(emptyBox), emptyLabel);
        gtk_box_append(GTK_BOX(emptyBox), emptyHint);
        gtk_list_box_append(GTK_LIST_BOX(m_searchResultsBox), emptyBox);
        return;
    }

    if (m_searchResults.isEmpty()) {
        auto* emptyBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_widget_set_margin_top(emptyBox, 12);
        gtk_widget_set_margin_bottom(emptyBox, 12);
        gtk_widget_set_margin_start(emptyBox, 12);
        gtk_widget_set_margin_end(emptyBox, 12);
        auto* emptyLabel = gtk_label_new("No packages found.");
        auto* emptyHint = gtk_label_new("Try a broader package name, or switch back to a local file.");
        gtk_label_set_xalign(GTK_LABEL(emptyLabel), 0.0f);
        gtk_label_set_xalign(GTK_LABEL(emptyHint), 0.0f);
        gtk_widget_add_css_class(emptyLabel, "dim-label");
        gtk_widget_add_css_class(emptyHint, "support-copy");
        gtk_box_append(GTK_BOX(emptyBox), emptyLabel);
        gtk_box_append(GTK_BOX(emptyBox), emptyHint);
        gtk_list_box_append(GTK_LIST_BOX(m_searchResultsBox), emptyBox);
        return;
    }

    for (int index = 0; index < m_searchResults.size(); ++index) {
        const auto& pkg = m_searchResults.at(index);
        auto* rowBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_widget_add_css_class(rowBox, "result-row");
        gtk_widget_set_margin_top(rowBox, 8);
        gtk_widget_set_margin_bottom(rowBox, 8);
        gtk_widget_set_margin_start(rowBox, 8);
        gtk_widget_set_margin_end(rowBox, 8);

        auto* textBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        auto* title = gtk_label_new(QString("%1  %2").arg(pkg.name, pkg.version).toUtf8().constData());
        gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
        gtk_widget_add_css_class(title, "section-title");
        const QString summaryText = QString("%1 • %2 • %3")
                                        .arg(pkg.repository)
                                        .arg(pkg.sizeFormatted())
                                        .arg(pkg.description);
        auto* summary = gtk_label_new(summaryText.toUtf8().constData());
        gtk_label_set_xalign(GTK_LABEL(summary), 0.0f);
        gtk_label_set_wrap(GTK_LABEL(summary), true);
        gtk_widget_add_css_class(summary, "dim-label");
        gtk_box_append(GTK_BOX(textBox), title);
        gtk_box_append(GTK_BOX(textBox), summary);

        auto* useButton = gtk_button_new_with_label("Download");
        gtk_widget_add_css_class(useButton, "pill-button");
        g_object_set_data(G_OBJECT(useButton), "pkg-index", GINT_TO_POINTER(index));
        g_signal_connect(useButton, "clicked", G_CALLBACK(AppWindow::onRepositoryDownloadClicked), this);

        gtk_box_append(GTK_BOX(rowBox), textBox);
        gtk_box_append(GTK_BOX(rowBox), useButton);
        gtk_widget_set_hexpand(textBox, true);

        auto* row = gtk_list_box_row_new();
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), rowBox);
        gtk_list_box_append(GTK_LIST_BOX(m_searchResultsBox), row);
    }
}

void AppWindow::startConversion() {
    if (m_running.load()) {
        requestCancel();
        return;
    }

    if (m_packagePaths.empty()) {
        appendLog("No package files selected.");
        return;
    }

    m_running = true;
    setProgress(0.0);
    setStatus("Preparing conversion...");
    appendLog("Starting conversion from GTK frontend.");
    updateActions();

    ConversionRequest request;
    request.packagePaths = toQStringList(m_packagePaths);
    request.outputDir = QString::fromUtf8(m_outputDir.c_str());
    request.optimizationSettings.enabled = gtk_switch_get_active(GTK_SWITCH(m_optimizeSwitch));
    request.optimizationSettings.compression = selectedCompression();
    request.dependencySettings.enabled = gtk_switch_get_active(GTK_SWITCH(m_dependencySwitch));

    std::thread([this, request]() mutable {
        ConversionController controller;

        {
            std::lock_guard<std::mutex> lock(m_controllerMutex);
            m_activeController = &controller;
        }

        QEventLoop loop;

        QObject::connect(&controller, &ConversionController::packageStarted,
                         [&controller, this](int index, int totalCount, const QString& packagePath) {
            const auto message = "Converting " + std::to_string(index + 1) + "/" + std::to_string(totalCount)
                + ": " + std::filesystem::path(toStdString(packagePath)).filename().string();
            runOnMain([this, message]() {
                setStatus(message);
                appendLog(message);
            });
        });

        QObject::connect(&controller, &ConversionController::progress,
                         [this](int percentage, const QString& message) {
            const auto status = toStdString(message);
            runOnMain([this, percentage, status]() {
                setProgress(static_cast<double>(percentage) / 100.0);
                setStatus(status);
            });
        });

        QObject::connect(&controller, &ConversionController::log,
                         [this](const QString& message) {
            const auto text = toStdString(message);
            runOnMain([this, text]() {
                appendLog(text);
            });
        });

        QObject::connect(&controller, &ConversionController::error,
                         [this](const QString& message) {
            const auto text = toStdString(message);
            runOnMain([this, text]() {
                setStatus(text);
                appendLog("ERROR: " + text);
            });
        });

        QObject::connect(&controller, &ConversionController::success,
                         [this](const QString& appImagePath) {
            const auto text = toStdString(appImagePath);
            runOnMain([this, text]() {
                appendLog("Created " + text);
                setStatus("AppImage created");
            });
        });

        QObject::connect(&controller, &ConversionController::finished,
                         [&loop, this](int successCount, int failureCount, bool cancelled) {
            runOnMain([this, successCount, failureCount, cancelled]() {
                m_running = false;
                if (cancelled) {
                    setStatus("Conversion cancelled.");
                    appendLog("Conversion cancelled.");
                } else if (failureCount == 0) {
                    setStatus("Done.");
                    appendLog("Completed successfully: " + std::to_string(successCount));
                } else {
                    setStatus("Completed with failures.");
                    appendLog("Completed with failures: " + std::to_string(successCount) + " ok, "
                              + std::to_string(failureCount) + " failed");
                }
                setProgress(cancelled ? 0.0 : 1.0);
                updateActions();
            });
            loop.quit();
        });

        QObject::connect(&controller, &ConversionController::sudoPasswordRequested,
                         [&controller, this](const QString& packagePath, const QString& reason) {
            const auto title = std::string("Sudo password required");
            const auto body = toStdString(reason) + "\n\nPackage: "
                + std::filesystem::path(toStdString(packagePath)).filename().string();

            runOnMain([this, &controller, title, body]() {
                requestSecret(title, body, [&controller, this](const std::string& value, bool accepted) {
                    if (!accepted || value.empty()) {
                        appendLog("Continuing without sudo password.");
                        QMetaObject::invokeMethod(&controller,
                                                  [&controller]() { controller.continueWithoutSudoPassword(); },
                                                  Qt::QueuedConnection);
                        return;
                    }

                    appendLog("Forwarding sudo password to conversion controller.");
                    QMetaObject::invokeMethod(&controller,
                                              [&controller, value]() {
                                                  controller.provideSudoPassword(QString::fromUtf8(value.c_str()));
                                              },
                                              Qt::QueuedConnection);
                });
            });
        });

        controller.start(request);
        loop.exec();

        {
            std::lock_guard<std::mutex> lock(m_controllerMutex);
            m_activeController = nullptr;
        }
    }).detach();
}

void AppWindow::startRepositorySearch() {
    const auto query = toStdString(gtk_editable_get_text(GTK_EDITABLE(m_searchEntry)));
    if (query.empty()) {
        updateSearchState("Enter a package name.");
        return;
    }

    m_isSearching = true;
    m_hasSearchRequest = true;
    m_searchResults.clear();
    refreshSearchResults();
    updateSearchState("Searching...");
    appendLog("Searching repositories for: " + query);
    updateActions();
    m_repositoryBrowser->searchPackagesAsync(QString::fromUtf8(query.c_str()));
}

void AppWindow::downloadRepositoryPackage(int index) {
    if (index < 0 || index >= m_searchResults.size()) {
        return;
    }

    const PackageInfo pkg = m_searchResults.at(index);
    const auto tempDir = std::filesystem::path(g_get_tmp_dir()) / "appalchemist-download";
    std::filesystem::create_directories(tempDir);

    auto startDownload = [this, pkg, tempDir]() {
        m_isDownloading = true;
        updateSearchState("Downloading " + toStdString(pkg.name) + "...");
        appendLog("Downloading package from repository: " + toStdString(pkg.name));
        updateActions();
        m_repositoryBrowser->downloadPackage(pkg, QString::fromUtf8(tempDir.string().c_str()));
    };

    if (pkg.source == PackageManager::PACMAN) {
        requestSecret(
            "Sudo password required",
            "Pacman downloads may require sudo.\n\nPackage: " + toStdString(pkg.name),
            [this, startDownload](const std::string& value, bool accepted) {
                if (!accepted || value.empty()) {
                    appendLog("Repository download cancelled: no sudo password provided.");
                    updateSearchState("Repository download cancelled.");
                    return;
                }

                m_repositoryBrowser->setSudoPassword(QString::fromUtf8(value.c_str()));
                startDownload();
            }
        );
        return;
    }

    startDownload();
}

void AppWindow::requestCancel() {
    std::lock_guard<std::mutex> lock(m_controllerMutex);
    if (!m_activeController) {
        return;
    }

    QMetaObject::invokeMethod(
        m_activeController,
        [controller = m_activeController]() {
            controller->cancel();
        },
        Qt::QueuedConnection
    );
}

void AppWindow::openOutputFolder() {
    const auto uri = std::string("file://") + m_outputDir;
    auto* launcher = gtk_uri_launcher_new(uri.c_str());
    gtk_uri_launcher_launch(launcher, GTK_WINDOW(m_window), nullptr, nullptr, nullptr);
    g_object_unref(launcher);
}

void AppWindow::chooseFiles() {
    auto* dialog = gtk_file_dialog_new();
    auto* filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    auto* filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Supported packages");
    gtk_file_filter_add_pattern(filter, "*.deb");
    gtk_file_filter_add_pattern(filter, "*.rpm");
    gtk_file_filter_add_pattern(filter, "*.tar.gz");
    gtk_file_filter_add_pattern(filter, "*.tar.xz");
    gtk_file_filter_add_pattern(filter, "*.tar.bz2");
    gtk_file_filter_add_pattern(filter, "*.tar.zst");
    gtk_file_filter_add_pattern(filter, "*.tgz");
    gtk_file_filter_add_pattern(filter, "*.txz");
    gtk_file_filter_add_pattern(filter, "*.zip");
    g_list_store_append(filters, filter);
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    gtk_file_dialog_open_multiple(dialog, GTK_WINDOW(m_window), nullptr, AppWindow::onFilesDialogFinished, this);
    g_object_unref(filter);
    g_object_unref(filters);
    g_object_unref(dialog);
}

void AppWindow::chooseOutputFolder() {
    auto* dialog = gtk_file_dialog_new();
    gtk_file_dialog_select_folder(dialog, GTK_WINDOW(m_window), nullptr, AppWindow::onOutputDialogFinished, this);
    g_object_unref(dialog);
}

void AppWindow::requestSecret(const std::string& title,
                              const std::string& body,
                              const std::function<void(const std::string&, bool)>& handler) {
    auto* dialog = adw_alert_dialog_new(title.c_str(), body.c_str());
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "skip", "Continue Without Password");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "confirm", "Use Password");
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "confirm", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "confirm");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "skip");
    adw_alert_dialog_set_prefer_wide_layout(ADW_ALERT_DIALOG(dialog), false);

    auto* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_add_css_class(root, "secret-dialog-box");
    auto* entry = gtk_password_entry_new();
    gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(entry), true);
    gtk_box_append(GTK_BOX(root), entry);
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), root);

    auto* state = new SecretDialogState{handler, GTK_EDITABLE(entry)};
    g_object_set_data_full(G_OBJECT(dialog), "secret-state", state, [](gpointer data) {
        delete static_cast<SecretDialogState*>(data);
    });

    adw_alert_dialog_choose(
        ADW_ALERT_DIALOG(dialog),
        m_window,
        nullptr,
        +[](GObject* sourceObject, GAsyncResult* result, gpointer) {
            auto* dialog = ADW_ALERT_DIALOG(sourceObject);
            auto* state = static_cast<SecretDialogState*>(g_object_get_data(G_OBJECT(dialog), "secret-state"));
            const char* response = adw_alert_dialog_choose_finish(dialog, result);
            if (!state) {
                return;
            }

            const bool accepted = response && g_strcmp0(response, "confirm") == 0;
            state->handler(accepted ? toStdString(gtk_editable_get_text(state->entry)) : "", accepted);
        },
        nullptr
    );
}

CompressionLevel AppWindow::selectedCompression() const {
    switch (gtk_drop_down_get_selected(GTK_DROP_DOWN(m_compressionDropdown))) {
        case 0:
            return CompressionLevel::FAST;
        case 2:
            return CompressionLevel::MAXIMUM;
        case 3:
            return CompressionLevel::ULTRA;
        default:
            return CompressionLevel::NORMAL;
    }
}

void AppWindow::onSelectFilesClicked(GtkButton*, gpointer userData) {
    static_cast<AppWindow*>(userData)->chooseFiles();
}

void AppWindow::onSelectOutputClicked(GtkButton*, gpointer userData) {
    static_cast<AppWindow*>(userData)->chooseOutputFolder();
}

void AppWindow::onConvertClicked(GtkButton*, gpointer userData) {
    static_cast<AppWindow*>(userData)->startConversion();
}

void AppWindow::onOpenOutputClicked(GtkButton*, gpointer userData) {
    static_cast<AppWindow*>(userData)->openOutputFolder();
}

void AppWindow::onSearchRepositoryClicked(GtkButton*, gpointer userData) {
    static_cast<AppWindow*>(userData)->startRepositorySearch();
}

void AppWindow::onRepositoryDownloadClicked(GtkButton* button, gpointer userData) {
    auto* self = static_cast<AppWindow*>(userData);
    self->downloadRepositoryPackage(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "pkg-index")));
}

void AppWindow::onToggleLogClicked(GtkButton*, gpointer userData) {
    static_cast<AppWindow*>(userData)->toggleLogVisibility();
}

gboolean AppWindow::onDropFiles(GtkDropTarget*, const GValue* value, double, double, gpointer userData) {
    auto* self = static_cast<AppWindow*>(userData);
    auto* files = static_cast<GdkFileList*>(g_value_get_boxed(value));
    if (!files) {
        return false;
    }

    self->m_packagePaths.clear();

    for (const GSList* item = gdk_file_list_get_files(files); item != nullptr; item = item->next) {
        auto* file = G_FILE(item->data);
        if (!file) {
            continue;
        }
        const auto path = filePathFromGFile(file);
        if (!path.empty()) {
            self->m_packagePaths.push_back(path);
        }
    }

    self->updateFileSummary();
    self->appendLog("Accepted files from drag and drop.");
    self->updateActions();
    return !self->m_packagePaths.empty();
}

void AppWindow::onFilesDialogFinished(GObject* sourceObject, GAsyncResult* result, gpointer userData) {
    auto* self = static_cast<AppWindow*>(userData);
    GError* error = nullptr;
    auto* files = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(sourceObject), result, &error);
    if (error) {
        g_error_free(error);
        return;
    }

    self->m_packagePaths.clear();
    for (guint index = 0; index < g_list_model_get_n_items(files); ++index) {
        auto* file = G_FILE(g_list_model_get_item(files, index));
        if (!file) {
            continue;
        }
        const auto path = filePathFromGFile(file);
        if (!path.empty()) {
            self->m_packagePaths.push_back(path);
        }
        g_object_unref(file);
    }

    g_object_unref(files);
    self->updateFileSummary();
    self->appendLog("Updated package selection.");
    self->updateActions();
}

void AppWindow::onOutputDialogFinished(GObject* sourceObject, GAsyncResult* result, gpointer userData) {
    auto* self = static_cast<AppWindow*>(userData);
    GError* error = nullptr;
    auto* folder = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(sourceObject), result, &error);
    if (error) {
        g_error_free(error);
        return;
    }

    self->m_outputDir = filePathFromGFile(folder);
    g_object_unref(folder);
    self->updateOutputSummary();
    self->appendLog("Output folder updated.");
}
