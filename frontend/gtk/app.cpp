#include "app.h"
#include "app_window.h"

GtkFrontendApp::GtkFrontendApp()
    : m_app(ADW_APPLICATION(adw_application_new("com.appalchemist.gtk", G_APPLICATION_DEFAULT_FLAGS)))
{
    g_signal_connect(m_app, "activate", G_CALLBACK(GtkFrontendApp::onActivate), this);
}

int GtkFrontendApp::run(int argc, char** argv) {
    return g_application_run(G_APPLICATION(m_app), argc, argv);
}

void GtkFrontendApp::onActivate(GApplication* app, gpointer) {
    auto* window = new AppWindow(ADW_APPLICATION(app));
    window->present();
}
