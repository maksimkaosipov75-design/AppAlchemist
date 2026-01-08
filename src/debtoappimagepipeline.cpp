#include "debtoappimagepipeline.h"
#include "utils.h"
#include <QStandardPaths>
#include <QDir>
#include <QDebug>
#include <QDateTime>
#include <QFileInfo>
#include <QMetaObject>

DebToAppImagePipeline::DebToAppImagePipeline(QObject* parent)
    : QObject(parent)
    , m_cancelled(false)
    , m_parser(new DebParser())
    , m_analyzer(new DependencyAnalyzer())
    , m_appDirBuilder(new AppDirBuilder())
    , m_appImageBuilder(new AppImageBuilder(this))
{
    // Forward log signals from AppImageBuilder
    connect(m_appImageBuilder, &AppImageBuilder::log, this, &DebToAppImagePipeline::log);
}

DebToAppImagePipeline::~DebToAppImagePipeline() {
    cleanup();
    delete m_parser;
    delete m_analyzer;
    delete m_appDirBuilder;
    delete m_appImageBuilder;
}

void DebToAppImagePipeline::setDebPath(const QString& debPath) {
    m_debPath = debPath;
}

void DebToAppImagePipeline::setOutputPath(const QString& outputPath) {
    m_outputPath = outputPath;
}

void DebToAppImagePipeline::start() {
    m_cancelled = false;
    QMetaObject::invokeMethod(this, "process", Qt::QueuedConnection);
}

void DebToAppImagePipeline::cancel() {
    m_cancelled = true;
    emit log("Operation cancelled by user");
}

void DebToAppImagePipeline::process() {
    emit progress(0, "Starting conversion...");
    emit log("=== Starting .deb to AppImage conversion ===");
    
    // Create temp directory
    QString tempBase = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    m_tempDir = QString("%1/deb-to-appimage-%2")
        .arg(tempBase)
        .arg(QString::number(QDateTime::currentMSecsSinceEpoch()));
    
    if (!SubprocessWrapper::createDirectory(m_tempDir)) {
        emit error("Failed to create temporary directory");
        emit finished();
        return;
    }
    
    emit log(QString("Using temporary directory: %1").arg(m_tempDir));
    
    // Step 1: Validate input
    if (m_cancelled) {
        emit finished();
        return;
    }
    
    emit progress(10, "Validating .deb file...");
    if (!validateInput()) {
        emit error("Invalid .deb file");
        cleanup();
        emit finished();
        return;
    }
    
    // Step 2: Extract .deb
    if (m_cancelled) {
        emit finished();
        return;
    }
    
    emit progress(20, "Extracting .deb package...");
    if (!extractDeb()) {
        emit error("Failed to extract .deb package or no executables found");
        cleanup();
        emit finished();
        return;
    }
    
    // Step 3: Analyze dependencies
    if (m_cancelled) {
        emit finished();
        return;
    }
    
    emit progress(40, "Analyzing dependencies...");
    if (!analyzeDependencies()) {
        emit error("Failed to analyze dependencies");
        cleanup();
        emit finished();
        return;
    }
    
    // Step 4: Build AppDir
    if (m_cancelled) {
        emit finished();
        return;
    }
    
    emit progress(60, "Building AppDir structure...");
    if (!buildAppDir()) {
        emit error("Failed to build AppDir");
        cleanup();
        emit finished();
        return;
    }
    
    // Step 5: Build AppImage
    if (m_cancelled) {
        emit finished();
        return;
    }
    
    emit progress(80, "Building AppImage...");
    if (!buildAppImage()) {
        QString errorMsg = "Failed to build AppImage.\n\n";
        if (m_appImageBuilder->findAppImageTool().isEmpty()) {
            errorMsg += "AppImageTool not found!\n\n";
            errorMsg += "Please download appimagetool from:\n";
            errorMsg += "https://github.com/AppImage/AppImageKit/releases\n\n";
            errorMsg += "And place it in:\n";
            errorMsg += "  - thirdparty/appimagetool (in project directory)\n";
            errorMsg += "  - /usr/bin/appimagetool (system-wide)\n";
            errorMsg += "  - /usr/local/bin/appimagetool (local)";
        } else {
            errorMsg += "Check the logs for details.";
        }
        emit error(errorMsg);
        cleanup();
        emit finished();
        return;
    }
    
    // Success
    emit progress(100, "Conversion completed successfully!");
    emit log("=== Conversion completed successfully ===");
    emit success(m_outputPath);
    
    cleanup();
    emit finished();
}

bool DebToAppImagePipeline::validateInput() {
    if (m_debPath.isEmpty() || !QFileInfo::exists(m_debPath)) {
        emit log("ERROR: .deb file not found");
        return false;
    }
    
    if (!m_parser->validateDebFile(m_debPath)) {
        emit log("ERROR: Invalid .deb file format");
        return false;
    }
    
    emit log(QString("Valid .deb file: %1").arg(m_debPath));
    return true;
}

bool DebToAppImagePipeline::extractDeb() {
    m_extractedDebDir = QString("%1/extracted").arg(m_tempDir);
    
    if (!m_parser->extractDeb(m_debPath, m_extractedDebDir)) {
        emit log("ERROR: Failed to extract .deb package");
        return false;
    }
    
    emit log("Successfully extracted .deb package");
    
    // Parse metadata
    m_metadata = m_parser->parseMetadata(m_extractedDebDir);
    emit log(QString("Package: %1, Version: %2").arg(m_metadata.package).arg(m_metadata.version));
    
    if (!m_metadata.scripts.isEmpty()) {
        emit log("WARNING: Package contains installation scripts (postinst/prerm). These will NOT be executed.");
        for (const QString& script : m_metadata.scripts) {
            emit log(QString("  - %1").arg(QFileInfo(script).fileName()));
        }
    }
    
    if (m_metadata.executables.isEmpty()) {
        emit log("ERROR: No executables found in package");
        emit log("Searched in: /usr/bin, /usr/sbin, /opt, /bin, /sbin, /usr/games, /usr/lib, /usr/libexec");
        
        // Check if this is a data package
        QString packageName = m_metadata.package.toLower();
        if (packageName.contains("-data") || packageName.contains("-common") || 
            packageName.contains("-dev") || packageName.contains("-doc")) {
            emit log("");
            emit log("TIP: This appears to be a data/common package, not the main application package.");
            emit log("     Data packages contain resources (icons, translations, data files) but no executables.");
            emit log("     You need to convert the MAIN package instead:");
            emit log(QString("     - For GIMP: use 'gimp' or 'gimp-bin' package, not 'gimp-data'"));
            emit log("     - For other apps: look for packages without '-data', '-common', '-dev', '-doc' suffixes");
        } else {
            emit log("Tip: The package might contain only libraries or data files, not executables");
        }
        return false;
    }
    
    emit log(QString("Found %1 executable(s)").arg(m_metadata.executables.size()));
    for (const QString& exec : m_metadata.executables) {
        emit log(QString("  - %1").arg(exec));
    }
    return true;
}

bool DebToAppImagePipeline::analyzeDependencies() {
    m_libraries = m_analyzer->collectLibraries(m_metadata.executables);
    emit log(QString("Found %1 library dependencies").arg(m_libraries.size()));
    
    // Check for system dependencies
    QStringList warnings = m_analyzer->checkSystemDependencies(m_metadata.depends);
    for (const QString& warning : warnings) {
        emit log(QString("WARNING: %1").arg(warning));
    }
    
    return true;
}

bool DebToAppImagePipeline::buildAppDir() {
    m_appDirPath = QString("%1/AppDir").arg(m_tempDir);
    
    emit log(QString("Building AppDir at: %1").arg(m_appDirPath));
    emit log(QString("Copying %1 executable(s)").arg(m_metadata.executables.size()));
    emit log(QString("Copying %1 library(ies)").arg(m_libraries.size()));
    
    if (!m_appDirBuilder->buildAppDir(m_appDirPath, m_extractedDebDir, m_metadata, m_libraries)) {
        emit log("ERROR: Failed to build AppDir");
        emit log("Check the logs above for specific error details");
        return false;
    }
    
    emit log("Successfully built AppDir structure");
    
    // Verify .desktop file exists
    QString desktopDir = QString("%1/usr/share/applications").arg(m_appDirPath);
    QDir desktopDirObj(desktopDir);
    if (desktopDirObj.exists()) {
        QStringList desktopFiles = desktopDirObj.entryList({"*.desktop"}, QDir::Files);
        if (desktopFiles.isEmpty()) {
            emit log("WARNING: No .desktop files found after building AppDir!");
            emit log(QString("Checked directory: %1").arg(desktopDir));
        } else {
            emit log(QString("Verified .desktop file(s) exist: %1").arg(desktopFiles.join(", ")));
            for (const QString& file : desktopFiles) {
                QString fullPath = desktopDirObj.absoluteFilePath(file);
                QFileInfo info(fullPath);
                emit log(QString("  - %1 (size: %2 bytes)").arg(file).arg(info.size()));
            }
        }
    } else {
        emit log(QString("ERROR: Desktop directory does not exist: %1").arg(desktopDir));
        return false;
    }
    
    return true;
}

bool DebToAppImagePipeline::buildAppImage() {
    if (m_outputPath.isEmpty()) {
        QFileInfo debInfo(m_debPath);
        QString baseName = debInfo.baseName();
        m_outputPath = QString("%1/%2.AppImage").arg(QDir::homePath()).arg(baseName);
    }
    
    emit log(QString("Building AppImage: %1").arg(m_outputPath));
    
    if (!m_appImageBuilder->buildAppImage(m_appDirPath, m_outputPath)) {
        emit log("ERROR: Failed to build AppImage.");
        // The error details are already logged by AppImageBuilder
        return false;
    }
    
    emit log(QString("Successfully created AppImage: %1").arg(m_outputPath));
    return true;
}

void DebToAppImagePipeline::cleanup() {
    if (!m_tempDir.isEmpty() && QDir(m_tempDir).exists()) {
        emit log("Cleaning up temporary files...");
        SubprocessWrapper::removeDirectory(m_tempDir);
    }
}

