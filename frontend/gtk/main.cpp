#include "app.h"
#include "cli_converter.h"
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFileInfo>
#include <QStringList>
#include <adwaita.h>

int main(int argc, char** argv) {
    QCoreApplication qtApp(argc, argv);
    qtApp.setApplicationName("AppAlchemist");
    qtApp.setApplicationVersion(APPALCHEMIST_VERSION);
    qtApp.setOrganizationName("AppAlchemist");

    QCommandLineParser parser;
    parser.setApplicationDescription("Convert .deb, .rpm, and archive packages to AppImage format");
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption convertOption(QStringList() << "c" << "convert",
        "Convert package file to AppImage (CLI mode)", "file");
    parser.addOption(convertOption);

    QCommandLineOption batchOption(QStringList() << "b" << "batch",
        "Batch convert multiple packages (specify files as positional arguments)");
    parser.addOption(batchOption);

    QCommandLineOption outputOption(QStringList() << "o" << "output",
        "Output directory for AppImage (default: ~/AppImages/)", "directory");
    parser.addOption(outputOption);

    QCommandLineOption noLaunchOption("no-launch",
        "Don't automatically launch AppImage after conversion");
    parser.addOption(noLaunchOption);

    parser.addPositionalArgument("files", "Package files to convert (.deb, .rpm, .tar.gz, .zip, etc.)", "[files...]");
    parser.process(qtApp);

    if (parser.isSet(batchOption)) {
        QStringList packagePaths = parser.positionalArguments();
        if (packagePaths.isEmpty()) {
            qCritical() << "Error: No package files specified for batch conversion";
            return 1;
        }

        QStringList validPaths;
        for (const QString& path : packagePaths) {
            QFileInfo info(path);
            if (info.exists()) {
                validPaths.append(info.absoluteFilePath());
            }
        }

        if (validPaths.isEmpty()) {
            qCritical() << "Error: No valid package files found";
            return 1;
        }

        CliConverter converter;
        return converter.convertBatch(validPaths,
                                      parser.value(outputOption),
                                      !parser.isSet(noLaunchOption));
    }

    QString packagePath;
    if (parser.isSet(convertOption)) {
        packagePath = parser.value(convertOption);
    } else if (!parser.positionalArguments().isEmpty()) {
        packagePath = parser.positionalArguments().first();
    }

    if (!packagePath.isEmpty()) {
        QFileInfo packageInfo(packagePath);
        if (!packageInfo.exists()) {
            qCritical() << "Error: Package file not found:" << packagePath;
            return 1;
        }

        CliConverter converter;
        return converter.convert(packagePath,
                                 parser.value(outputOption),
                                 !parser.isSet(noLaunchOption));
    }

    adw_init();
    GtkFrontendApp app;
    return app.run(argc, argv);
}
