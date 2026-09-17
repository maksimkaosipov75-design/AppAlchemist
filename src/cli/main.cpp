#include "cli_converter.h"
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFileInfo>
#include <QStringList>
#include <QDebug>
#include <iostream>

#ifndef APPALCHEMIST_VERSION
#define APPALCHEMIST_VERSION "1.5.0"
#endif

int main(int argc, char** argv) {
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext&, const QString& msg) {
        std::cerr << msg.toStdString() << "\n";
    });

    QCoreApplication qtApp(argc, argv);
    qtApp.setApplicationName("AppAlchemist CLI");
    qtApp.setApplicationVersion(APPALCHEMIST_VERSION);
    qtApp.setOrganizationName("AppAlchemist");

    QCommandLineParser parser;
    parser.setApplicationDescription("AppAlchemist Headless CLI: Convert .deb, .rpm, and archive packages to AppImage format");
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

    QCommandLineOption quietOption(QStringList() << "q" << "quiet",
        "Quiet mode: suppress progress output");
    parser.addOption(quietOption);

    QCommandLineOption jsonOption("json",
        "Output progress and status in JSON format");
    parser.addOption(jsonOption);

    QCommandLineOption dryRunOption("dry-run",
        "Inspect package without building AppImage");
    parser.addOption(dryRunOption);

    QCommandLineOption bundleDepsOption("bundle-deps",
        "Fetch the package's declared dependencies and bundle their contents (requires network)");
    parser.addOption(bundleDepsOption);

    parser.addPositionalArgument("files", "Package files to convert (.deb, .rpm, .tar.gz, .zip, etc.)", "[files...]");

    if (!parser.parse(QCoreApplication::arguments())) {
        std::cerr << "Usage error: " << parser.errorText().toStdString() << "\n";
        return 2;
    }

    if (parser.isSet("help")) {
        std::cout << parser.helpText().toStdString();
        return 0;
    }

    if (parser.isSet("version")) {
        std::cout << QString("%1 %2\n").arg(qtApp.applicationName(), qtApp.applicationVersion()).toStdString();
        return 0;
    }

    CliOptions opts;
    opts.json = parser.isSet(jsonOption);
    opts.quiet = parser.isSet(quietOption);
    opts.dryRun = parser.isSet(dryRunOption);
    opts.bundleDependencies = parser.isSet(bundleDepsOption);
    opts.autoLaunch = !parser.isSet(noLaunchOption);
    opts.outputDir = parser.value(outputOption);

    if (parser.isSet(batchOption)) {
        opts.isBatch = true;
        opts.batchPaths = parser.positionalArguments();
        if (opts.batchPaths.isEmpty()) {
            std::cerr << "Usage error: No package files specified for batch conversion.\n";
            return 2;
        }

        CliConverter converter;
        return converter.convertBatch(opts);
    }

    if (parser.isSet(convertOption)) {
        opts.packagePath = parser.value(convertOption);
    } else if (!parser.positionalArguments().isEmpty()) {
        opts.packagePath = parser.positionalArguments().first();
    }

    if (opts.packagePath.isEmpty()) {
        std::cerr << "Usage error: No package file specified.\nUse --help for usage information.\n";
        return 2;
    }

    CliConverter converter;
    return converter.convert(opts);
}
