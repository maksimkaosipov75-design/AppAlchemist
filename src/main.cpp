#include "appmainwindow.h"
#include "cli_converter.h"
#include <QApplication>
#include <QStyleFactory>
#include <QDir>
#include <QCommandLineParser>
#include <QCommandLineOption>
#include <QFileInfo>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    
    app.setApplicationName("AppAlchemist");
    app.setApplicationVersion("1.0.0");
    app.setOrganizationName("AppAlchemist");
    
    // Parse command line arguments
    QCommandLineParser parser;
    parser.setApplicationDescription("Convert .deb and .rpm packages to AppImage format");
    parser.addHelpOption();
    parser.addVersionOption();
    
    QCommandLineOption convertOption(QStringList() << "c" << "convert",
        "Convert package file to AppImage (CLI mode)", "file");
    parser.addOption(convertOption);
    
    QCommandLineOption outputOption(QStringList() << "o" << "output",
        "Output directory for AppImage (default: ~/AppImages/)", "directory");
    parser.addOption(outputOption);
    
    QCommandLineOption noLaunchOption("no-launch",
        "Don't automatically launch AppImage after conversion");
    parser.addOption(noLaunchOption);
    
    parser.process(app);
    
    // Check if we should run in CLI mode
    if (parser.isSet(convertOption)) {
        QString packagePath = parser.value(convertOption);
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

