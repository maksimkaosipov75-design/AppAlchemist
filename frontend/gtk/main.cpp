#include "app.h"
#include <QCoreApplication>
#include <adwaita.h>

int main(int argc, char** argv) {
    QCoreApplication qtApp(argc, argv);
    qtApp.setApplicationName("AppAlchemist");
    qtApp.setApplicationVersion(APPALCHEMIST_VERSION);
    qtApp.setOrganizationName("AppAlchemist");

    adw_init();
    GtkFrontendApp app;
    return app.run(argc, argv);
}
