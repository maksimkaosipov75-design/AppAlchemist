#ifndef APPALCHEMIST_GTK_APP_H
#define APPALCHEMIST_GTK_APP_H

#include <adwaita.h>

class GtkFrontendApp {
public:
    GtkFrontendApp();
    int run(int argc, char** argv);

private:
    static void onActivate(GApplication* app, gpointer userData);

    AdwApplication* m_app;
};

#endif // APPALCHEMIST_GTK_APP_H
