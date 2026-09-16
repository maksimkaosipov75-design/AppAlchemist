#include "app.h"
#include "app_window.h"

GtkFrontendApp::GtkFrontendApp()
    : m_app(ADW_APPLICATION(adw_application_new("com.appalchemist.gtk", G_APPLICATION_DEFAULT_FLAGS)))
{
    g_signal_connect(m_app, "activate", G_CALLBACK(GtkFrontendApp::onActivate), this);
}

GtkFrontendApp::~GtkFrontendApp() = default;

int GtkFrontendApp::run(int argc, char** argv) {
    const int status = g_application_run(G_APPLICATION(m_app), argc, argv);
    // Join conversion workers now that no GTK dispatch can be blocked by it.
    m_windows.clear();
    return status;
}

void GtkFrontendApp::onActivate(GApplication* app, gpointer userData) {
    auto* self = static_cast<GtkFrontendApp*>(userData);
    auto window = std::make_unique<AppWindow>(ADW_APPLICATION(app));
    window->present();
    self->m_windows.push_back(std::move(window));
}
