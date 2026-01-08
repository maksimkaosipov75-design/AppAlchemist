#include "cli_converter.h"
#include <QApplication>
#include <QFileInfo>
#include <QDir>
#include <QProcess>
#include <QStandardPaths>
#include <QDateTime>
#include <QEventLoop>
#include <QTimer>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QRegularExpressionMatch>
#include <QTextStream>
#include <algorithm>

CliConverter::CliConverter(QObject* parent)
    : QObject(parent)
    , m_pipeline(nullptr)
    , m_pipelineThread(nullptr)
    , m_logFile(nullptr)
    , m_logStream(nullptr)
    , m_autoLaunch(true)
    , m_success(false)
{
    setupLogging();
}

CliConverter::~CliConverter() {
    if (m_pipelineThread) {
        m_pipelineThread->quit();
        m_pipelineThread->wait();
        delete m_pipelineThread;
    }
    if (m_pipeline) {
        delete m_pipeline;
    }
    if (m_logStream) {
        delete m_logStream;
    }
    if (m_logFile) {
        m_logFile->close();
        delete m_logFile;
    }
}

void CliConverter::setupLogging() {
    QString logDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/appalchemist/logs";
    QDir dir;
    if (!dir.exists(logDir)) {
        dir.mkpath(logDir);
    }
    
    QString logFilePath = QString("%1/appalchemist-%2.log")
        .arg(logDir)
        .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd"));
    
    m_logFile = new QFile(logFilePath);
    if (m_logFile->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        m_logStream = new QTextStream(m_logFile);
        *m_logStream << "\n=== Session started at " << QDateTime::currentDateTime().toString(Qt::ISODate) << " ===\n";
        m_logStream->flush();
    }
}

void CliConverter::logToFile(const QString& message) {
    if (m_logStream) {
        *m_logStream << QDateTime::currentDateTime().toString("[yyyy-MM-dd hh:mm:ss] ") << message << "\n";
        m_logStream->flush();
    }
}

void CliConverter::sendNotification(const QString& title, const QString& message, const QString& urgency) {
    QStringList args;
    if (urgency == "error" || urgency == "critical") {
        args << "--urgency=critical";
    } else {
        args << "--urgency=normal";
    }
    args << "--app-name=AppAlchemist";
    args << title;
    args << message;
    
    // Try to send notification (silently fail if notify-send is not available)
    SubprocessWrapper::execute("notify-send", args, {}, 5000);
}

int CliConverter::convert(const QString& packagePath, const QString& outputDir, bool autoLaunch) {
    QElapsedTimer timer;
    timer.start();
    
    m_packagePath = packagePath;
    m_outputDir = outputDir;
    m_autoLaunch = autoLaunch;
    m_success = false;
    m_resultAppImagePath.clear();
    
    QFileInfo packageInfo(packagePath);
    if (!packageInfo.exists()) {
        QString errorMsg = QString("Package file not found: %1").arg(packagePath);
        logToFile("ERROR: " + errorMsg);
        sendNotification("AppAlchemist Error", errorMsg, "error");
        return 1;
    }
    
    logToFile(QString("Starting conversion: %1").arg(packagePath));
    sendNotification("AppAlchemist", QString("Converting %1...").arg(packageInfo.fileName()), "normal");
    
    // Check cache first
    QString cachedAppImage = CacheManager::getValidCachedAppImage(packagePath);
    if (!cachedAppImage.isEmpty()) {
        logToFile(QString("Using cached AppImage: %1").arg(cachedAppImage));
        sendNotification("AppAlchemist", QString("Using cached AppImage for %1").arg(packageInfo.fileName()), "normal");
        
        m_resultAppImagePath = cachedAppImage;
        m_success = true;
        
        // Always create desktop entry for cached AppImage too
        createDesktopEntry(cachedAppImage);
        
        if (m_autoLaunch) {
            if (launchAppImage(cachedAppImage)) {
                return 0;
            } else {
                return 1;
            }
        }
        return 0;
    }
    
    // Determine output path
    QString appImagePath = determineAppImagePath(packagePath, outputDir);
    
    // Create pipeline in separate thread
    m_pipelineThread = new QThread(this);
    m_pipeline = new PackageToAppImagePipeline();
    m_pipeline->moveToThread(m_pipelineThread);
    
    connect(m_pipelineThread, &QThread::started, m_pipeline, &PackageToAppImagePipeline::start);
    // Use QueuedConnection for cross-thread signals to ensure proper delivery
    connect(m_pipeline, &PackageToAppImagePipeline::progress, this, &CliConverter::onProgress, Qt::QueuedConnection);
    connect(m_pipeline, &PackageToAppImagePipeline::log, this, &CliConverter::onLog, Qt::QueuedConnection);
    connect(m_pipeline, &PackageToAppImagePipeline::error, this, &CliConverter::onError, Qt::QueuedConnection);
    connect(m_pipeline, &PackageToAppImagePipeline::success, this, &CliConverter::onSuccess, Qt::QueuedConnection);
    connect(m_pipeline, &PackageToAppImagePipeline::finished, this, &CliConverter::onPipelineFinished, Qt::QueuedConnection);
    connect(m_pipelineThread, &QThread::finished, m_pipeline, &QObject::deleteLater);
    
    // Set paths
    m_pipeline->setPackagePath(packagePath);
    m_pipeline->setOutputPath(appImagePath);
    
    // Start conversion
    m_pipelineThread->start();
    
    // Wait for completion (blocking)
    // Use QEventLoop to process events properly while waiting
    QEventLoop loop;
    QMetaObject::Connection conn = connect(m_pipelineThread, &QThread::finished, &loop, &QEventLoop::quit);
    
    // Process events until thread finishes
    int waitCount = 0;
    while (m_pipelineThread->isRunning()) {
        loop.processEvents(QEventLoop::AllEvents, 100);
        QThread::msleep(10); // Small delay to allow signal processing
        waitCount++;
        if (waitCount > 1000) { // Timeout after ~10 seconds
            logToFile("WARNING: Thread still running after timeout, forcing wait");
            break;
        }
        // Double check in case signal was missed
        if (!m_pipelineThread->isRunning()) {
            break;
        }
    }
    
    // Disconnect and ensure thread is really finished
    disconnect(conn);
    if (m_pipelineThread->isRunning()) {
        logToFile("Thread still running, waiting up to 5 seconds...");
        m_pipelineThread->wait(5000); // Wait up to 5 seconds
    }
    
    // Process all pending events multiple times to ensure signals are handled
    // Use the main event loop to process queued signals
    logToFile("Processing pending events...");
    for (int i = 0; i < 30; ++i) {
        QApplication::processEvents(QEventLoop::AllEvents, 200);
        QThread::msleep(50); // Small delay between event processing
    }
    
    logToFile(QString("Pipeline finished. success=%1, autoLaunch=%2, appImagePath='%3'")
              .arg(m_success).arg(m_autoLaunch).arg(m_resultAppImagePath));
    
    // Force check: if we have a result path but success is false, check if file exists
    if (!m_success && !m_resultAppImagePath.isEmpty()) {
        QFileInfo resultInfo(m_resultAppImagePath);
        if (resultInfo.exists()) {
            logToFile("WARNING: m_success is false but AppImage exists, setting success=true");
            m_success = true;
        }
    }
    
    // Additional check: if onSuccess was called but we missed it
    if (!m_success && !m_resultAppImagePath.isEmpty()) {
        logToFile("Double-checking: AppImage path is set but success is false");
        QFileInfo resultInfo(m_resultAppImagePath);
        if (resultInfo.exists() && resultInfo.size() > 0) {
            logToFile("AppImage exists and has size, setting success=true");
            m_success = true;
        }
    }
    
    if (m_success && !m_resultAppImagePath.isEmpty()) {
        qint64 elapsed = timer.elapsed();
        logToFile(QString("Conversion successful in %1 ms. autoLaunch=%2, appImagePath='%3'").arg(elapsed).arg(m_autoLaunch).arg(m_resultAppImagePath));
        
        // Create desktop entry synchronously to ensure it's done before returning
        createDesktopEntry(m_resultAppImagePath);
        
        // Always try to launch if autoLaunch is enabled
        if (m_autoLaunch) {
            logToFile("Attempting to launch AppImage...");
            // Use a small delay to ensure AppImage file is fully written
            QThread::msleep(500);
            if (launchAppImage(m_resultAppImagePath)) {
                logToFile("AppImage launched successfully");
                // Play sound notification for fast conversions
                if (elapsed < 2000) {
                    SubprocessWrapper::execute("paplay", {"/usr/share/sounds/freedesktop/stereo/complete.oga"}, {}, 1000);
                }
                return 0;
            } else {
                logToFile("Failed to launch AppImage, but conversion was successful");
                // Don't return error code if conversion succeeded
                return 0;
            }
        } else {
            logToFile("Auto-launch disabled (--no-launch flag)");
        }
        return 0;
    }
    
    if (!m_success) {
        logToFile("Conversion failed or was cancelled");
        return 1;
    }
    
    if (m_resultAppImagePath.isEmpty()) {
        logToFile("WARNING: AppImage path is empty, cannot launch");
        return 1;
    }
    
    return 0;
}

QString CliConverter::determineAppImagePath(const QString& packagePath, const QString& customOutputDir) {
    if (!customOutputDir.isEmpty()) {
        QFileInfo packageInfo(packagePath);
        QString baseName = packageInfo.baseName();
        return QString("%1/%2.AppImage").arg(customOutputDir).arg(baseName);
    }
    
    // Use cache directory
    return CacheManager::getAppImagePath(packagePath);
}

void CliConverter::onProgress(int percentage, const QString& message) {
    QString logMsg = QString("[%1%] %2").arg(percentage).arg(message);
    logToFile(logMsg);
    // Don't spam notifications for every progress update
}

void CliConverter::onLog(const QString& message) {
    logToFile(message);
}

void CliConverter::onError(const QString& errorMessage) {
    logToFile("ERROR: " + errorMessage);
    sendNotification("AppAlchemist Error", errorMessage, "error");
    m_success = false;
}

void CliConverter::onSuccess(const QString& appImagePath) {
    logToFile(QString("SUCCESS: AppImage created at %1").arg(appImagePath));
    sendNotification("AppAlchemist", QString("Successfully converted to AppImage"), "normal");
    // Set success flag and path immediately
    m_success = true;
    m_resultAppImagePath = appImagePath;
    logToFile(QString("onSuccess: Set m_success=true, m_resultAppImagePath='%1'").arg(appImagePath));
}

void CliConverter::onPipelineFinished() {
    // Cleanup is handled in destructor
}

void CliConverter::createDesktopEntry(const QString& appImagePath) {
    logToFile(QString("Creating desktop entry for: %1").arg(appImagePath));
    
    // Extract AppImage to get .desktop file
    QString tempDir = QString("/tmp/appalchemist-desktop-%1").arg(QDateTime::currentMSecsSinceEpoch());
    QDir tempDirObj(tempDir);
    if (!tempDirObj.exists()) {
        tempDirObj.mkpath(".");
    }
    
    // Extract AppImage
    QString extractCommand = QString("\"%1\" --appimage-extract").arg(appImagePath);
    QProcess extractProcess;
    extractProcess.setWorkingDirectory(tempDir);
    extractProcess.start("/bin/sh", QStringList() << "-c" << extractCommand);
    if (!extractProcess.waitForFinished(30000)) {
        logToFile("WARNING: Failed to extract AppImage for desktop entry");
        tempDirObj.removeRecursively();
        return;
    }
    
    // Find .desktop file in extracted AppImage
    // First try usr/share/applications (standard location)
    QString desktopFile;
    QDir squashfsRoot(QString("%1/squashfs-root").arg(tempDir));
    
    // Get expected app name from AppImage filename
    QFileInfo appImageInfo(appImagePath);
    QString expectedAppName = appImageInfo.baseName().toLower();
    // Remove common suffixes
    expectedAppName.replace(QRegularExpression("(-|_)(latest|current|stable|installer|linux|amd64|x86_64|all|appimage)$", QRegularExpression::CaseInsensitiveOption), "");
    
    if (squashfsRoot.exists()) {
        QDir applicationsDir(QString("%1/usr/share/applications").arg(squashfsRoot.absolutePath()));
        if (applicationsDir.exists()) {
            QStringList desktopFiles = applicationsDir.entryList({"*.desktop"}, QDir::Files);
            
            // First pass: try to find exact match or close match by name
            QStringList candidates;
            for (const QString& file : desktopFiles) {
                QString filePath = applicationsDir.absoluteFilePath(file);
                QFile f(filePath);
                if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                    QString content = f.readAll();
                    f.close();
                    
                    // Skip URL handlers, templates, and other non-main entries
                    QString fileLower = file.toLower();
                    QFileInfo fileInfo(file);
                    QString baseName = fileInfo.baseName().toLower();
                    // Skip if contains template-like patterns (numbers, texture, size indicators, etc.)
                    bool looksLikeTemplate = baseName.contains(QRegularExpression("\\d+k|\\d+bit|texture|\\d+x\\d+|template|example|sample|canon|nikon|dslr"));
                    if (content.contains("NoDisplay=true") || content.contains("MimeType=") || 
                        fileLower.contains("url-handler") || fileLower.contains("handler") ||
                        fileLower.contains("template") || fileLower.contains("example") ||
                        fileLower.contains("sample") || fileLower.contains("data/") ||
                        looksLikeTemplate || !content.contains("Type=Application")) {
                        continue;
                    }
                    
                    // Check if filename matches expected app name
                    if (baseName == expectedAppName || baseName.contains(expectedAppName) || expectedAppName.contains(baseName)) {
                        desktopFile = filePath;
                        logToFile(QString("Found matching desktop file by name: %1").arg(file));
                        break;
                    }
                    
                    candidates.append(filePath);
                }
            }
            
            // Second pass: if no exact match, use shortest candidate (main apps usually have shorter names)
            // But exclude files that look like templates
            if (desktopFile.isEmpty() && !candidates.isEmpty()) {
                // Filter out template-like files
                QStringList filteredCandidates;
                for (const QString& candidate : candidates) {
                    QString baseName = QFileInfo(candidate).baseName().toLower();
                    bool looksLikeTemplate = baseName.contains(QRegularExpression("\\d+k|\\d+bit|texture|\\d+x\\d+|template|example|sample|canon|nikon|dslr"));
                    if (!looksLikeTemplate) {
                        filteredCandidates.append(candidate);
                    }
                }
                // If all were filtered, use original candidates
                if (filteredCandidates.isEmpty()) {
                    filteredCandidates = candidates;
                }
                // Sort by filename length (shorter = more likely to be main app)
                std::sort(filteredCandidates.begin(), filteredCandidates.end(), [](const QString& a, const QString& b) {
                    return QFileInfo(a).baseName().length() < QFileInfo(b).baseName().length();
                });
                desktopFile = filteredCandidates.first();
                logToFile(QString("Using shortest desktop file: %1").arg(QFileInfo(desktopFile).fileName()));
            }
        }
        
        // Fallback to root directory (but exclude subdirectories like data/, templates/, etc.)
        // Actually, skip root directory entirely - if no desktop file in usr/share/applications,
        // create one from executable instead
        if (desktopFile.isEmpty()) {
            logToFile("No desktop file found in usr/share/applications, will create from executable");
        }
    }
    
    QString desktopContent;
    if (desktopFile.isEmpty()) {
        logToFile("WARNING: No main .desktop file found in AppImage, creating one from AppRun or executable");
        // Try to find executable in usr/bin that matches AppImage name
        QFileInfo appImageInfo(appImagePath);
        QString expectedAppName = appImageInfo.baseName();
        // Remove common suffixes
        expectedAppName.replace(QRegularExpression("(-|_)(latest|current|stable|installer|linux|amd64|x86_64|all|AppImage)$", QRegularExpression::CaseInsensitiveOption), "");
        QString expectedExecutable = expectedAppName.toLower();
        
        QString foundExecutable;
        QDir usrBinDir(QString("%1/usr/bin").arg(squashfsRoot.absolutePath()));
        if (usrBinDir.exists()) {
            QStringList executables = usrBinDir.entryList(QDir::Files | QDir::Executable);
            // Try exact match first
            for (const QString& exe : executables) {
                if (exe.toLower() == expectedExecutable) {
                    foundExecutable = exe;
                    logToFile(QString("Found matching executable: %1").arg(exe));
                    break;
                }
            }
            // Try partial match
            if (foundExecutable.isEmpty()) {
                for (const QString& exe : executables) {
                    QString exeLower = exe.toLower();
                    if (exeLower.contains(expectedExecutable) || expectedExecutable.contains(exeLower)) {
                        foundExecutable = exe;
                        logToFile(QString("Found partially matching executable: %1").arg(exe));
                        break;
                    }
                }
            }
            // Use first executable if no match found
            if (foundExecutable.isEmpty() && !executables.isEmpty()) {
                // Filter out scripts and build tools
                for (const QString& exe : executables) {
                    QString exeLower = exe.toLower();
                    if (!exeLower.contains("build") && !exeLower.contains("script") && 
                        !exeLower.contains(".sh") && !exeLower.contains(".vdf")) {
                        foundExecutable = exe;
                        logToFile(QString("Using first non-script executable: %1").arg(exe));
                        break;
                    }
                }
                // If still empty, use first executable
                if (foundExecutable.isEmpty()) {
                    foundExecutable = executables.first();
                    logToFile(QString("Using first executable: %1").arg(foundExecutable));
                }
            }
        }
        
        // Create a basic desktop entry
        QString appName = expectedAppName;
        appName.replace("-", " ");
        appName.replace("_", " ");
        // Capitalize first letter of each word
        QStringList words = appName.split(" ", Qt::SkipEmptyParts);
        if (!words.isEmpty()) {
            for (int i = 0; i < words.size(); i++) {
                if (!words[i].isEmpty()) {
                    words[i] = words[i].left(1).toUpper() + words[i].mid(1).toLower();
                }
            }
            appName = words.join(" ");
        } else {
            appName = "Application";
        }
        
        // Create desktop content first, then extract and install icon
        desktopContent = QString("[Desktop Entry]\n"
                                "Type=Application\n"
                                "Name=%1\n"
                                "Exec=%2\n"
                                "TryExec=%2\n"
                                "Categories=Utility;\n"
                                "Terminal=false\n")
                                .arg(appName)
                                .arg(appImagePath);
        
        // Extract and install icon before adding Icon= line
        QString iconName = extractAndInstallIcon(squashfsRoot, appImagePath, desktopContent);
        if (!iconName.isEmpty()) {
            desktopContent += QString("Icon=%1\n").arg(iconName);
        }
        
        logToFile(QString("Created desktop entry for: %1 (executable: %2)").arg(appName).arg(foundExecutable));
    } else {
        // Read and modify .desktop file
        QFile sourceDesktop(desktopFile);
        if (!sourceDesktop.open(QIODevice::ReadOnly | QIODevice::Text)) {
            logToFile("WARNING: Failed to read .desktop file from AppImage");
            tempDirObj.removeRecursively();
            return;
        }
        
        desktopContent = sourceDesktop.readAll();
        sourceDesktop.close();
    }
    
    // Modify Exec line to point to AppImage
    QRegularExpression execRegex("^Exec=(.+)$", QRegularExpression::MultilineOption);
    QRegularExpressionMatch execMatch = execRegex.match(desktopContent);
    if (execMatch.hasMatch()) {
        QString oldExec = execMatch.captured(1);
        // Replace with AppImage path
        desktopContent.replace(execRegex, QString("Exec=%1").arg(appImagePath));
        logToFile(QString("Updated Exec line: %1").arg(appImagePath));
    } else {
        // Add Exec line if missing
        desktopContent += QString("\nExec=%1\n").arg(appImagePath);
    }
    
    // Ensure TryExec points to AppImage
    QRegularExpression tryExecRegex("^TryExec=(.+)$", QRegularExpression::MultilineOption);
    if (tryExecRegex.match(desktopContent).hasMatch()) {
        desktopContent.replace(tryExecRegex, QString("TryExec=%1").arg(appImagePath));
    } else {
        desktopContent += QString("TryExec=%1\n").arg(appImagePath);
    }
    
    // Remove or fix Path line - it should not point to paths inside AppImage
    QRegularExpression pathRegex("^Path=(.+)$", QRegularExpression::MultilineOption);
    if (pathRegex.match(desktopContent).hasMatch()) {
        // Remove Path line as it points to non-existent system path
        desktopContent.remove(pathRegex);
        logToFile("Removed Path line from desktop entry (points to AppImage internal path)");
    }
    
    // Extract and install icon
    QString iconName = extractAndInstallIcon(squashfsRoot, appImagePath, desktopContent);
    
    // Update Icon line in desktop content
    QRegularExpression iconRegex("^Icon=(.+)$", QRegularExpression::MultilineOption);
    if (iconRegex.match(desktopContent).hasMatch()) {
        desktopContent.replace(iconRegex, QString("Icon=%1").arg(iconName));
        logToFile(QString("Updated Icon line: %1").arg(iconName));
    } else if (!iconName.isEmpty()) {
        desktopContent += QString("Icon=%1\n").arg(iconName);
    }
    
    // Get application name from .desktop file or use AppImage filename
    QString appName;
    QRegularExpression nameRegex("^Name=(.+)$", QRegularExpression::MultilineOption);
    QRegularExpressionMatch nameMatch = nameRegex.match(desktopContent);
    if (nameMatch.hasMatch()) {
        appName = nameMatch.captured(1).trimmed();
    } else {
        QFileInfo appImageInfo(appImagePath);
        appName = appImageInfo.baseName();
        desktopContent += QString("Name=%1\n").arg(appName);
    }
    
    // Create target desktop file path
    QString desktopDir = QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation);
    QDir desktopDirObj(desktopDir);
    if (!desktopDirObj.exists()) {
        desktopDirObj.mkpath(".");
    }
    
    // Generate safe filename from app name
    QString safeName = appName;
    safeName.replace(QRegularExpression("[^a-zA-Z0-9_-]"), "-");
    safeName = safeName.toLower();
    if (safeName.isEmpty()) {
        QFileInfo appImageInfo(appImagePath);
        safeName = appImageInfo.baseName().toLower();
    }
    
    QString targetDesktopPath = QString("%1/%2.desktop").arg(desktopDir).arg(safeName);
    
    // Write desktop file
    QFile targetDesktop(targetDesktopPath);
    if (!targetDesktop.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        logToFile(QString("WARNING: Failed to write desktop file: %1").arg(targetDesktopPath));
        tempDirObj.removeRecursively();
        return;
    }
    
    QTextStream out(&targetDesktop);
    out << desktopContent;
    targetDesktop.close();
    
    // Make desktop file executable (required by some desktop environments)
    QFile::setPermissions(targetDesktopPath, QFile::ReadUser | QFile::WriteUser | QFile::ExeUser | 
                                                 QFile::ReadGroup | QFile::ExeGroup | 
                                                 QFile::ReadOther | QFile::ExeOther);
    
    logToFile(QString("Desktop entry created: %1").arg(targetDesktopPath));
    
    // Update desktop database
    QProcess updateProcess;
    updateProcess.start("update-desktop-database", QStringList() << desktopDir);
    if (updateProcess.waitForFinished(5000)) {
        logToFile("Desktop database updated successfully");
    } else {
        logToFile("WARNING: Failed to update desktop database (may not be critical)");
    }
    
    // Cleanup
    tempDirObj.removeRecursively();
    
    sendNotification("AppAlchemist", QString("Application added to menu: %1").arg(appName), "normal");
}

QString CliConverter::extractAndInstallIcon(const QDir& squashfsRoot, const QString& appImagePath, const QString& desktopContent) {
    // Extract icon name from desktop content
    QString iconName;
    QRegularExpression iconRegex("^Icon=(.+)$", QRegularExpression::MultilineOption);
    QRegularExpressionMatch iconMatch = iconRegex.match(desktopContent);
    if (iconMatch.hasMatch()) {
        iconName = iconMatch.captured(1).trimmed();
        // Remove path, keep only name
        iconName = QFileInfo(iconName).baseName();
    }
    
    // If no icon name from desktop, try to get from AppImage filename
    if (iconName.isEmpty()) {
        QFileInfo appImageInfo(appImagePath);
        iconName = appImageInfo.baseName();
        // Remove common suffixes
        iconName.replace(QRegularExpression("(-|_)(latest|current|stable|installer|linux|amd64|x86_64|all|AppImage)$", QRegularExpression::CaseInsensitiveOption), "");
    }
    
    // Try common icon names (vscodium, codium, etc.)
    QStringList iconNamesToTry = {iconName, "vscodium", "codium", "code"};
    if (iconName != "vscodium") iconNamesToTry.prepend("vscodium");
    if (iconName != "codium") iconNamesToTry.prepend("codium");
    
    // Try to find icon in AppImage
    QStringList iconExtensions = {"png", "svg", "xpm", "ico"};
    QStringList iconSizes = {"256x256", "128x128", "64x64", "48x48", "32x32", "16x16"};
    QString foundIconPath;
    
    // First try usr/share/icons/hicolor/*/apps/ with all possible icon names
    for (const QString& tryIconName : iconNamesToTry) {
        for (const QString& size : iconSizes) {
            for (const QString& ext : iconExtensions) {
                QString iconPath = QString("%1/usr/share/icons/hicolor/%2/apps/%3.%4")
                    .arg(squashfsRoot.absolutePath()).arg(size).arg(tryIconName).arg(ext);
                if (QFile::exists(iconPath)) {
                    foundIconPath = iconPath;
                    iconName = tryIconName; // Update iconName to the found one
                    logToFile(QString("Found icon: %1").arg(iconPath));
                    break;
                }
            }
            if (!foundIconPath.isEmpty()) break;
        }
        if (!foundIconPath.isEmpty()) break;
    }
    
    // Try usr/share/pixmaps/ with all possible icon names
    if (foundIconPath.isEmpty()) {
        for (const QString& tryIconName : iconNamesToTry) {
            for (const QString& ext : iconExtensions) {
                QString iconPath = QString("%1/usr/share/pixmaps/%2.%3")
                    .arg(squashfsRoot.absolutePath()).arg(tryIconName).arg(ext);
                if (QFile::exists(iconPath)) {
                    foundIconPath = iconPath;
                    iconName = tryIconName; // Update iconName to the found one
                    logToFile(QString("Found icon in pixmaps: %1").arg(iconPath));
                    break;
                }
            }
            if (!foundIconPath.isEmpty()) break;
        }
    }
    
    // Try root directory with all possible icon names
    if (foundIconPath.isEmpty()) {
        for (const QString& tryIconName : iconNamesToTry) {
            for (const QString& ext : iconExtensions) {
                QString iconPath = QString("%1/%2.%3")
                    .arg(squashfsRoot.absolutePath()).arg(tryIconName).arg(ext);
                if (QFile::exists(iconPath)) {
                    foundIconPath = iconPath;
                    iconName = tryIconName; // Update iconName to the found one
                    logToFile(QString("Found icon in root: %1").arg(iconPath));
                    break;
                }
            }
            if (!foundIconPath.isEmpty()) break;
        }
    }
    
    // Try .DirIcon (might be a symlink) - but only if it doesn't point to a template
    if (foundIconPath.isEmpty()) {
        QString dirIconPath = QString("%1/.DirIcon").arg(squashfsRoot.absolutePath());
        if (QFile::exists(dirIconPath)) {
            QFileInfo dirIconInfo(dirIconPath);
            QString potentialIconPath;
            QString potentialIconName;
            
            if (dirIconInfo.isSymLink()) {
                // Follow symlink
                QString symlinkTarget = dirIconInfo.symLinkTarget();
                // Try absolute path first
                if (QFile::exists(symlinkTarget)) {
                    potentialIconPath = symlinkTarget;
                } else {
                    // Try relative path from squashfs-root
                    QString relativeTarget = QString("%1/%2").arg(squashfsRoot.absolutePath()).arg(symlinkTarget);
                    if (QFile::exists(relativeTarget)) {
                        potentialIconPath = relativeTarget;
                    } else {
                        // Try just the filename in root
                        QString fileName = QFileInfo(symlinkTarget).fileName();
                        QString rootTarget = QString("%1/%2").arg(squashfsRoot.absolutePath()).arg(fileName);
                        if (QFile::exists(rootTarget)) {
                            potentialIconPath = rootTarget;
                        }
                    }
                }
                // Update icon name from symlink target
                if (!potentialIconPath.isEmpty()) {
                    QFileInfo targetInfo(potentialIconPath);
                    potentialIconName = targetInfo.baseName();
                }
            } else {
                potentialIconPath = dirIconPath;
            }
            
            // Check if the icon is a template (contains numbers, texture, etc.)
            if (!potentialIconPath.isEmpty() && !potentialIconName.isEmpty()) {
                QString iconNameLower = potentialIconName.toLower();
                bool looksLikeTemplate = iconNameLower.contains(QRegularExpression("\\d+k|\\d+bit|texture|\\d+x\\d+|template|example|sample|canon|nikon|dslr"));
                if (!looksLikeTemplate) {
                    foundIconPath = potentialIconPath;
                    iconName = potentialIconName;
                    logToFile(QString("Found .DirIcon: %1").arg(foundIconPath));
                } else {
                    logToFile(QString("Skipping .DirIcon (looks like template): %1").arg(potentialIconName));
                }
            } else if (!potentialIconPath.isEmpty()) {
                // If we can't determine name, check the path
                QString pathLower = potentialIconPath.toLower();
                bool looksLikeTemplate = pathLower.contains(QRegularExpression("\\d+k|\\d+bit|texture|\\d+x\\d+|template|example|sample|canon|nikon|dslr"));
                if (!looksLikeTemplate) {
                    foundIconPath = potentialIconPath;
                    logToFile(QString("Found .DirIcon: %1").arg(foundIconPath));
                } else {
                    logToFile(QString("Skipping .DirIcon (path looks like template): %1").arg(potentialIconPath));
                }
            }
        }
    }
    
    if (foundIconPath.isEmpty()) {
        logToFile(QString("WARNING: No icon found for %1").arg(iconName));
        return iconName; // Return icon name anyway, system might have it
    }
    
    // Determine icon extension and name
    QFileInfo iconInfo(foundIconPath);
    QString iconExt = iconInfo.suffix();
    if (iconExt.isEmpty()) {
        // Check if it's .DirIcon (might be PNG)
        if (foundIconPath.endsWith(".DirIcon")) {
            iconExt = "png";
        } else {
            iconExt = "png"; // Default
        }
    }
    
    // If we found .DirIcon but iconName is still generic, try to find vscodium.png
    if (foundIconPath.contains(".DirIcon") && (iconName.isEmpty() || iconName == "codium")) {
        // Check if vscodium.png exists in root
        QString vscodiumPath = QString("%1/vscodium.png").arg(squashfsRoot.absolutePath());
        if (QFile::exists(vscodiumPath)) {
            iconName = "vscodium";
            foundIconPath = vscodiumPath;
            iconExt = "png";
            logToFile(QString("Using vscodium.png as icon"));
        }
    }
    
    // Install icon to ~/.local/share/icons/hicolor/*/apps/
    QString iconsBaseDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/icons";
    QDir iconsDir(iconsBaseDir);
    if (!iconsDir.exists()) {
        iconsDir.mkpath(".");
    }
    
    bool iconInstalled = false;
    
    // If icon is from hicolor, copy to all sizes
    if (foundIconPath.contains("/hicolor/")) {
        for (const QString& size : iconSizes) {
            QString targetDir = QString("%1/hicolor/%2/apps").arg(iconsBaseDir).arg(size);
            QDir targetDirObj(targetDir);
            if (!targetDirObj.exists()) {
                targetDirObj.mkpath(".");
            }
            QString targetPath = QString("%1/%2.%3").arg(targetDir).arg(iconName).arg(iconExt);
            if (QFile::copy(foundIconPath, targetPath)) {
                iconInstalled = true;
                logToFile(QString("Installed icon: %1").arg(targetPath));
            }
        }
    } else {
        // Copy to 256x256 (default size)
        QString targetDir = QString("%1/hicolor/256x256/apps").arg(iconsBaseDir);
        QDir targetDirObj(targetDir);
        if (!targetDirObj.exists()) {
            targetDirObj.mkpath(".");
        }
        QString targetPath = QString("%1/%2.%3").arg(targetDir).arg(iconName).arg(iconExt);
        if (QFile::copy(foundIconPath, targetPath)) {
            iconInstalled = true;
            logToFile(QString("Installed icon: %1").arg(targetPath));
            
            // Also copy to other sizes if possible
            for (const QString& size : iconSizes) {
                if (size != "256x256") {
                    QString targetDirSize = QString("%1/hicolor/%2/apps").arg(iconsBaseDir).arg(size);
                    QDir targetDirObjSize(targetDirSize);
                    if (!targetDirObjSize.exists()) {
                        targetDirObjSize.mkpath(".");
                    }
                    QString targetPathSize = QString("%1/%2.%3").arg(targetDirSize).arg(iconName).arg(iconExt);
                    QFile::copy(foundIconPath, targetPathSize);
                }
            }
        }
    }
    
    if (iconInstalled) {
        // Update icon cache
        QProcess updateIconCache;
        updateIconCache.start("gtk-update-icon-cache", QStringList() << "-f" << "-t" << QString("%1/hicolor").arg(iconsBaseDir));
        if (updateIconCache.waitForFinished(5000)) {
            logToFile("Icon cache updated");
        } else {
            logToFile("WARNING: Failed to update icon cache (may not be critical)");
        }
    }
    
    return iconName;
}

bool CliConverter::launchAppImage(const QString& appImagePath) {
    logToFile(QString("launchAppImage called with path: %1").arg(appImagePath));
    
    QFileInfo appImageInfo(appImagePath);
    if (!appImageInfo.exists()) {
        QString errorMsg = QString("AppImage not found: %1").arg(appImagePath);
        logToFile("ERROR: " + errorMsg);
        sendNotification("AppAlchemist Error", errorMsg, "error");
        return false;
    }
    
    logToFile(QString("AppImage exists: %1, size: %2 bytes").arg(appImagePath).arg(appImageInfo.size()));
    
    // Always ensure executable permissions
    if (!appImageInfo.isExecutable()) {
        logToFile("AppImage is not executable, making it executable...");
        if (!SubprocessWrapper::setExecutable(appImagePath)) {
            logToFile("WARNING: Failed to set executable permissions, trying anyway...");
        } else {
            logToFile("Successfully set executable permissions");
        }
    }
    
    QString absolutePath = appImageInfo.absoluteFilePath();
    logToFile(QString("Launching AppImage: %1").arg(absolutePath));
    
    // Try multiple launch methods for maximum compatibility
    // Method 1: QProcess::startDetached with shell
    bool started = QProcess::startDetached("/bin/sh", QStringList() << "-c" << QString("\"%1\" &").arg(absolutePath));
    
    if (started) {
        logToFile("AppImage launched successfully using QProcess::startDetached with shell");
        sendNotification("AppAlchemist", QString("Launching %1...").arg(QFileInfo(appImagePath).fileName()), "normal");
        return true;
    }
    
    logToFile("Method 1 failed, trying QProcess::startDetached directly...");
    
    // Method 2: QProcess::startDetached directly
    started = QProcess::startDetached(absolutePath, QStringList());
    if (started) {
        logToFile("AppImage launched successfully using QProcess::startDetached");
        sendNotification("AppAlchemist", QString("Launching %1...").arg(QFileInfo(appImagePath).fileName()), "normal");
        return true;
    }
    
    logToFile("Method 2 failed, trying system()...");
    
    // Method 3: system() with nohup and disown
    QString command = QString("nohup \"%1\" > /dev/null 2>&1 & disown").arg(absolutePath);
    int result = system(command.toLocal8Bit().constData());
    if (result == 0) {
        logToFile("AppImage launched successfully using system() with nohup");
        sendNotification("AppAlchemist", QString("Launching %1...").arg(QFileInfo(appImagePath).fileName()), "normal");
        return true;
    }
    
    logToFile("Method 3 failed, trying simple system()...");
    
    // Method 4: Simple system() call
    command = QString("\"%1\" &").arg(absolutePath);
    result = system(command.toLocal8Bit().constData());
    if (result == 0) {
        logToFile("AppImage launched successfully using simple system()");
        sendNotification("AppAlchemist", QString("Launching %1...").arg(QFileInfo(appImagePath).fileName()), "normal");
        return true;
    }
    
    // All methods failed
    logToFile(QString("All launch methods failed. Last error code: %1").arg(result));
    sendNotification("AppAlchemist Error", QString("Failed to launch %1").arg(QFileInfo(appImagePath).fileName()), "error");
    return false;
}

