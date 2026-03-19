#include "app.h"
#include <QCoreApplication>
#include <adwaita.h>

int main(int argc, char** argv) {
    QCoreApplication qtApp(argc, argv);
    adw_init();

    GtkFrontendApp app;
    return app.run(argc, argv);
}
