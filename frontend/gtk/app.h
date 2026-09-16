#ifndef APPALCHEMIST_GTK_APP_H
#define APPALCHEMIST_GTK_APP_H

#include <adwaita.h>
#include <memory>
#include <vector>

class AppWindow;

class GtkFrontendApp {
public:
    GtkFrontendApp();
    ~GtkFrontendApp();

    GtkFrontendApp(const GtkFrontendApp&) = delete;
    GtkFrontendApp& operator=(const GtkFrontendApp&) = delete;

    int run(int argc, char** argv);

private:
    static void onActivate(GApplication* app, gpointer userData);

    AdwApplication* m_app;
    // Windows are kept alive until the main loop has returned: their
    // destructors join the conversion worker, which must not happen while the
    // GTK loop is still dispatching.
    std::vector<std::unique_ptr<AppWindow>> m_windows;
};

#endif // APPALCHEMIST_GTK_APP_H
