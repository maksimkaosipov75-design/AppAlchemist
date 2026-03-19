#include "appmainwindow.h"
#include "cli_converter.h"
#include "tarballparser.h"
// #include "store/store_window.h" // Removed - not used
#include <QApplication>
#include <QStyleFactory>
#include <QDir>
#include <QCommandLineParser>
#include <QCommandLineOption>
#include <QFileInfo>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    
    app.setApplicationName("AppAlchemist");
    app.setApplicationVersion("1.2.0");
    app.setOrganizationName("AppAlchemist");
    
    // Parse command line arguments
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

    QCommandLineOption storeOption(QStringList() << "s" << "store",
        "Open the application store");
    parser.addOption(storeOption);

    parser.addPositionalArgument("files", "Package files to convert (.deb, .rpm, .tar.gz, .zip, etc.)", "[files...]");
    
    parser.process(app);

    // Check for store mode
    if (parser.isSet(storeOption)) {
        qCritical() << "Store mode is not available in this build";
        qInfo() << "Use appalchemist-store instead for store functionality";
        return 1;
    }

    // Check for batch mode
    if (parser.isSet(batchOption)) {
        QStringList packagePaths = parser.positionalArguments();
        
        if (packagePaths.isEmpty()) {
            qCritical() << "Error: No package files specified for batch conversion";
            qInfo() << "Usage: appalchemist --batch file1.deb file2.rpm file3.tar.gz -o ~/AppImages/";
            return 1;
        }
        
        // Validate all files exist
        QStringList validPaths;
        for (const QString& path : packagePaths) {
            QFileInfo info(path);
            if (!info.exists()) {
                qWarning() << "Warning: File not found, skipping:" << path;
            } else {
                validPaths.append(info.absoluteFilePath());
            }
        }
        
        if (validPaths.isEmpty()) {
            qCritical() << "Error: No valid package files found";
            return 1;
        }
        
        QString outputDir = parser.value(outputOption);
        bool autoLaunch = !parser.isSet(noLaunchOption);
        
        // Run batch conversion
        CliConverter converter;
        int exitCode = converter.convertBatch(validPaths, outputDir, autoLaunch);
        return exitCode;
    }
    
    // Check if we should run in CLI mode (single file)
    QString packagePath;
    if (parser.isSet(convertOption)) {
        packagePath = parser.value(convertOption);
    } else if (parser.positionalArguments().size() > 0) {
        // Support opening file directly without --convert flag (for desktop file handlers)
        packagePath = parser.positionalArguments().first();
    }
    
    if (!packagePath.isEmpty()) {
        QFileInfo packageInfo(packagePath);
        
        if (!packageInfo.exists()) {
            qCritical() << "Error: Package file not found:" << packagePath;
            return 1;
        }
        
        QString outputDir = parser.value(outputOption);
        bool autoLaunch = !parser.isSet(noLaunchOption);
        
        // Run in CLI mode
        CliConverter converter;
        int exitCode = converter.convert(packagePath, outputDir, autoLaunch);
        return exitCode;
    }
    
    // Run in GUI mode
    app.setStyle(QStyleFactory::create("Fusion"));
    
    AppMainWindow window;
    
    // Remove system menu button in top left corner (must be done before show())
    // Use explicit window flags without system menu
    Qt::WindowFlags flags = Qt::Window | 
                            Qt::WindowTitleHint | 
                            Qt::WindowMinimizeButtonHint | 
                            Qt::WindowMaximizeButtonHint | 
                            Qt::WindowCloseButtonHint;
    window.setWindowFlags(flags);
    
    window.show();
    
    return app.exec();
}

