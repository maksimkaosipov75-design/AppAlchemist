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
    , m_fileSummaryLabel(nullptr)
    , m_outputSummaryLabel(nullptr)
    , m_statusLabel(nullptr)
    , m_progressBar(nullptr)
    , m_logView(nullptr)
    , m_selectFilesButton(nullptr)
    , m_selectOutputButton(nullptr)
    , m_convertButton(nullptr)
    , m_openOutputButton(nullptr)
    , m_optimizeSwitch(nullptr)
    , m_dependencySwitch(nullptr)
    , m_compressionDropdown(nullptr)
    , m_outputDir(defaultOutputDir())
    , m_running(false)
    , m_activeController(nullptr)
{
    buildUi();
    loadCss();
    updateFileSummary();
    updateOutputSummary();
    updateActions();
}

void AppWindow::present() {
    gtk_window_present(GTK_WINDOW(m_window));
}

void AppWindow::buildUi() {
    m_window = adw_application_window_new(GTK_APPLICATION(m_app));
    gtk_window_set_title(GTK_WINDOW(m_window), "AppAlchemist");
    gtk_window_set_default_size(GTK_WINDOW(m_window), 980, 760);

    auto* toolbarView = adw_toolbar_view_new();
    auto* headerBar = adw_header_bar_new();
    auto* titleWidget = adw_window_title_new("AppAlchemist", "Minimal GNOME frontend");
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(headerBar), titleWidget);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbarView), headerBar);

    auto* scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbarView), scroller);

    auto* clamp = adw_clamp_new();
    adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 860);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), clamp);

    auto* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 24);
    gtk_widget_set_margin_top(root, 24);
    gtk_widget_set_margin_bottom(root, 24);
    gtk_widget_set_margin_start(root, 24);
    gtk_widget_set_margin_end(root, 24);
    adw_clamp_set_child(ADW_CLAMP(clamp), root);

    m_dropCard = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_add_css_class(m_dropCard, "drop-card");
    auto* dropIcon = gtk_image_new_from_icon_name("package-x-generic-symbolic");
    gtk_widget_set_size_request(dropIcon, 56, 56);
    auto* dropTitle = gtk_label_new("Select or drop packages");
    gtk_widget_add_css_class(dropTitle, "hero-title");
    gtk_label_set_xalign(GTK_LABEL(dropTitle), 0.0f);
    auto* dropSubtitle = gtk_label_new("Single-purpose layout, system accent color, no decorative noise.");
    gtk_widget_add_css_class(dropSubtitle, "hero-subtitle");
    gtk_label_set_xalign(GTK_LABEL(dropSubtitle), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(dropSubtitle), true);
    m_fileSummaryLabel = gtk_label_new("");
    gtk_widget_add_css_class(m_fileSummaryLabel, "hero-meta");
    gtk_label_set_xalign(GTK_LABEL(m_fileSummaryLabel), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(m_fileSummaryLabel), true);
    auto* heroActions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    m_selectFilesButton = gtk_button_new_with_label("Choose Packages");
    gtk_widget_add_css_class(m_selectFilesButton, "suggested-action");
    auto* clearButton = gtk_button_new_with_label("Clear");
    gtk_box_append(GTK_BOX(heroActions), m_selectFilesButton);
    gtk_box_append(GTK_BOX(heroActions), clearButton);
    gtk_box_append(GTK_BOX(m_dropCard), dropIcon);
    gtk_box_append(GTK_BOX(m_dropCard), dropTitle);
    gtk_box_append(GTK_BOX(m_dropCard), dropSubtitle);
    gtk_box_append(GTK_BOX(m_dropCard), m_fileSummaryLabel);
    gtk_box_append(GTK_BOX(m_dropCard), heroActions);
    gtk_box_append(GTK_BOX(root), m_dropCard);

    auto* target = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
    g_signal_connect(target, "drop", G_CALLBACK(AppWindow::onDropFiles), this);
    gtk_widget_add_controller(m_dropCard, GTK_EVENT_CONTROLLER(target));

    auto* settingsGroup = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(settingsGroup), "Conversion");

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
    adw_action_row_set_subtitle(ADW_ACTION_ROW(depsRow), "Temporarily disabled until GTK password flow is implemented.");
    m_dependencySwitch = gtk_switch_new();
    gtk_widget_set_sensitive(m_dependencySwitch, false);
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

    gtk_box_append(GTK_BOX(root), settingsGroup);

    auto* statusCard = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(statusCard, "status-card");
    m_statusLabel = gtk_label_new("Ready");
    gtk_label_set_xalign(GTK_LABEL(m_statusLabel), 0.0f);
    gtk_widget_add_css_class(m_statusLabel, "status-label");
    m_progressBar = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(m_progressBar), false);
    gtk_box_append(GTK_BOX(statusCard), m_statusLabel);
    gtk_box_append(GTK_BOX(statusCard), m_progressBar);
    gtk_box_append(GTK_BOX(root), statusCard);

    auto* actionRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    m_convertButton = gtk_button_new_with_label("Build AppImage");
    gtk_widget_add_css_class(m_convertButton, "suggested-action");
    m_openOutputButton = gtk_button_new_with_label("Open Output Folder");
    gtk_box_append(GTK_BOX(actionRow), m_convertButton);
    gtk_box_append(GTK_BOX(actionRow), m_openOutputButton);
    gtk_box_append(GTK_BOX(root), actionRow);

    auto* logCard = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_add_css_class(logCard, "log-card");
    auto* logTitle = gtk_label_new("Activity");
    gtk_label_set_xalign(GTK_LABEL(logTitle), 0.0f);
    gtk_widget_add_css_class(logTitle, "section-title");
    auto* logScroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(logScroll), 220);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(logScroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    m_logView = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(m_logView), false);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(m_logView), false);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(m_logView), true);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(logScroll), m_logView);
    gtk_box_append(GTK_BOX(logCard), logTitle);
    gtk_box_append(GTK_BOX(logCard), logScroll);
    gtk_box_append(GTK_BOX(root), logCard);

    gtk_window_set_child(GTK_WINDOW(m_window), toolbarView);

    g_signal_connect(m_selectFilesButton, "clicked", G_CALLBACK(AppWindow::onSelectFilesClicked), this);
    g_signal_connect(m_selectOutputButton, "clicked", G_CALLBACK(AppWindow::onSelectOutputClicked), this);
    g_signal_connect(m_convertButton, "clicked", G_CALLBACK(AppWindow::onConvertClicked), this);
    g_signal_connect(m_openOutputButton, "clicked", G_CALLBACK(AppWindow::onOpenOutputClicked), this);
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
        summary = "No packages selected";
    } else if (m_packagePaths.size() == 1) {
        summary = std::filesystem::path(m_packagePaths.front()).filename().string();
    } else {
        summary = std::to_string(m_packagePaths.size()) + " packages selected";
    }

    gtk_label_set_text(GTK_LABEL(m_fileSummaryLabel), summary.c_str());
}

void AppWindow::updateOutputSummary() {
    gtk_label_set_text(GTK_LABEL(m_outputSummaryLabel), m_outputDir.c_str());
}

void AppWindow::updateActions() {
    const bool hasPackages = !m_packagePaths.empty();
    const bool running = m_running.load();

    gtk_widget_set_sensitive(m_selectFilesButton, !running);
    gtk_widget_set_sensitive(m_selectOutputButton, !running);
    gtk_widget_set_sensitive(m_openOutputButton, !running);
    gtk_widget_set_sensitive(m_optimizeSwitch, !running);
    gtk_widget_set_sensitive(m_compressionDropdown, !running);
    gtk_button_set_label(GTK_BUTTON(m_convertButton), running ? "Cancel" : "Build AppImage");
    gtk_widget_set_sensitive(m_convertButton, running || hasPackages);
}

void AppWindow::appendLog(const std::string& message) {
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
    request.dependencySettings.enabled = false;

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

        controller.start(request);
        loop.exec();

        {
            std::lock_guard<std::mutex> lock(m_controllerMutex);
            m_activeController = nullptr;
        }
    }).detach();
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
