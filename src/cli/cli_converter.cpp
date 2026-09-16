#include "cli_converter.h"
#include <QCoreApplication>
#include <QFileInfo>
#include <QDir>
#include <QDirIterator>
#include <QProcess>
#include <QStandardPaths>
#include <QDateTime>
#include <QEventLoop>
#include <QTimer>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QRegularExpressionMatch>
#include <QTextStream>
#include <QVector>
#include <QJsonObject>
#include <QJsonDocument>
#include <iostream>
#include <algorithm>
#include <limits>

namespace {

QString normalizeDesktopMatchName(QString value) {
    value = value.toLower();
    value.replace(QRegularExpression("\\.desktop$"), "");
    value.replace(QRegularExpression("(-|_)(latest|current|stable|installer|linux|amd64|x86_64|all|appimage)$",
                                     QRegularExpression::CaseInsensitiveOption), "");
    return value;
}

QStringList buildIconNameVariants(const QString& name) {
    const QString original = name.trimmed();
    if (original.isEmpty()) {
        return {};
    }

    QStringList variants;
    auto addVariant = [&variants](const QString& candidate) {
        QString simplified = candidate.trimmed();
        if (!simplified.isEmpty() && !variants.contains(simplified)) {
            variants.append(simplified);
        }
    };

    addVariant(original);
    addVariant(original.toLower());
    addVariant(QString(original).replace('.', '-'));
    addVariant(QString(original).toLower().replace('.', '-'));
    addVariant(QString(original).replace('.', '_'));
    addVariant(QString(original).toLower().replace('.', '_'));

    return variants;
}

QString legacySafeDesktopName(const QString& appName) {
    QString safeName = appName;
    safeName.replace(QRegularExpression("[^a-zA-Z0-9_-]"), "-");
    return safeName.toLower();
}

QString normalizeDesktopIdentifier(QString value) {
    value = value.toLower();
    value.replace(QRegularExpression("[^a-z0-9]"), "");
    return value;
}

int scoreIconAsset(const QString& iconPath) {
    const QString lowerPath = iconPath.toLower();
    if (lowerPath.contains("1x1") || lowerPath.contains("placeholder")) {
        return -1000;
    }

    if (lowerPath.endsWith(".png")) {
        QFile pngFile(iconPath);
        if (pngFile.open(QIODevice::ReadOnly)) {
            const QByteArray header = pngFile.read(24);
            pngFile.close();
            if (header.size() >= 24 && header.startsWith("\x89PNG\r\n\x1a\n")) {
                auto readBigEndian32 = [](const char* data) -> int {
                    return (static_cast<unsigned char>(data[0]) << 24) |
                           (static_cast<unsigned char>(data[1]) << 16) |
                           (static_cast<unsigned char>(data[2]) << 8) |
                           static_cast<unsigned char>(data[3]);
                };
                const int width = readBigEndian32(header.constData() + 16);
                const int height = readBigEndian32(header.constData() + 20);
                if (width <= 1 || height <= 1) {
                    return -1000;
                }
                return std::max(width, height);
            }
        }
    }

    if (lowerPath.endsWith(".svg")) {
        QFile svgFile(iconPath);
        if (svgFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QString svgContent = QString::fromUtf8(svgFile.read(512));
            svgFile.close();
            if (svgContent.contains("width=\"1\"", Qt::CaseInsensitive) ||
                svgContent.contains("height=\"1\"", Qt::CaseInsensitive)) {
                return -1000;
            }
        }
    }

    QRegularExpression tinyIconRegex("(^|[^0-9])(1)x(1)([^0-9]|$)");
    if (tinyIconRegex.match(lowerPath).hasMatch()) {
        return -1000;
    }

    QRegularExpression sizedPathRegex("(\\d+)x(\\d+)");
    const QRegularExpressionMatch sizedPathMatch = sizedPathRegex.match(lowerPath);
    if (sizedPathMatch.hasMatch()) {
        const int width = sizedPathMatch.captured(1).toInt();
        const int height = sizedPathMatch.captured(2).toInt();
        if (width <= 1 || height <= 1) {
            return -1000;
        }
        return std::max(width, height);
    }

    QRegularExpression suffixedSizeRegex("_(\\d+)\\.(png|svg|xpm|ico)$");
    const QRegularExpressionMatch suffixedSizeMatch = suffixedSizeRegex.match(lowerPath);
    if (suffixedSizeMatch.hasMatch()) {
        return suffixedSizeMatch.captured(1).toInt();
    }

    if (lowerPath.endsWith(".svg")) {
        return 512;
    }

    return 64;
}

QString selectBestIconAsset(const QStringList& candidatePaths, const QStringList& preferredNames) {
    QString bestPath;
    int bestScore = std::numeric_limits<int>::min();

    for (const QString& candidatePath : candidatePaths) {
        const QFileInfo info(candidatePath);
        if (!info.exists() || !info.isFile()) {
            continue;
        }

        int score = scoreIconAsset(candidatePath);
        if (score < 0) {
            continue;
        }

        const QString lowerFileName = info.fileName().toLower();
        for (int index = 0; index < preferredNames.size(); ++index) {
            const QString preferred = preferredNames.at(index).toLower();
            if (preferred.isEmpty()) {
                continue;
            }

            if (lowerFileName == QString("%1.%2").arg(preferred, info.suffix().toLower())) {
                score += 200 - (index * 10);
            } else if (lowerFileName.contains(preferred)) {
                score += 120 - (index * 10);
            }
        }

        if (bestPath.isEmpty() || score > bestScore) {
            bestPath = candidatePath;
            bestScore = score;
        }
    }

    return bestPath;
}

bool copyFileReplacing(const QString& sourcePath, const QString& targetPath) {
    if (QFile::exists(targetPath) && !QFile::remove(targetPath)) {
        return false;
    }
    return QFile::copy(sourcePath, targetPath);
}

}

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
    cleanupPipelineObjects();
    if (m_logStream) {
        delete m_logStream;
    }
    if (m_logFile) {
        m_logFile->close();
        delete m_logFile;
    }
}

void CliConverter::cleanupPipelineObjects() {
    if (m_pipelineThread) {
        if (m_pipelineThread->isRunning()) {
            m_pipelineThread->quit();
            m_pipelineThread->wait();
        }
        delete m_pipelineThread;
        m_pipelineThread = nullptr;
    }

    m_pipeline = nullptr;
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

int CliConverter::convert(const CliOptions& options) {
    cleanupPipelineObjects();

    QElapsedTimer timer;
    timer.start();
    
    m_packagePath = options.packagePath;
    m_outputDir = options.outputDir;
    m_autoLaunch = options.autoLaunch;
    m_json = options.json;
    m_quiet = options.quiet;
    m_dryRun = options.dryRun;
    m_success = false;
    m_lastError.clear();
    m_resultAppImagePath.clear();
    
    QFileInfo packageInfo(m_packagePath);
    if (m_packagePath.isEmpty() || !packageInfo.exists()) {
        QString errorMsg = m_packagePath.isEmpty() ? "No package file specified" : QString("Package file not found: %1").arg(m_packagePath);
        m_lastError = errorMsg;
        logToFile("ERROR: " + errorMsg);
        if (m_json) {
            QJsonObject errObj;
            errObj["event"] = "error";
            errObj["message"] = errorMsg;
            std::cout << QJsonDocument(errObj).toJson(QJsonDocument::Compact).toStdString() << "\n";
            QJsonObject compObj;
            compObj["event"] = "complete";
            compObj["success"] = false;
            compObj["error"] = errorMsg;
            compObj["dry_run"] = m_dryRun;
            std::cout << QJsonDocument(compObj).toJson(QJsonDocument::Compact).toStdString() << "\n";
            std::cout.flush();
        }
        std::cerr << "Error: " << errorMsg.toStdString() << "\n";
        if (!m_quiet && !m_json) {
            sendNotification("AppAlchemist Error", errorMsg, "error");
        }
        return 1;
    }
    
    logToFile(QString("Starting conversion: %1").arg(m_packagePath));
    if (!m_quiet && !m_json) {
        sendNotification("AppAlchemist", QString("Converting %1...").arg(packageInfo.fileName()), "normal");
    }
    
    // Check cache first (skip in dry-run mode)
    if (!m_dryRun) {
        QString cachedAppImage = CacheManager::getValidCachedAppImage(m_packagePath);
        if (!cachedAppImage.isEmpty()) {
            // A cache hit must still honour the requested output location:
            // otherwise the CLI reports success while the caller's -o
            // directory stays empty and the path printed on stdout points
            // into an unrelated directory from an earlier run.
            const QString requestedPath = determineAppImagePath(m_packagePath, m_outputDir);
            if (!requestedPath.isEmpty()
                && QFileInfo(requestedPath).absoluteFilePath()
                       != QFileInfo(cachedAppImage).absoluteFilePath()) {
                if (!placeCachedAppImage(cachedAppImage, requestedPath)) {
                    logToFile(QString("Could not publish cached AppImage to %1; rebuilding.")
                                  .arg(requestedPath));
                    cachedAppImage.clear();
                } else {
                    cachedAppImage = requestedPath;
                }
            }
        }
        if (!cachedAppImage.isEmpty()) {
            logToFile(QString("Using cached AppImage: %1").arg(cachedAppImage));
            const CachedConversionMetadata metadata = CacheManager::getConversionMetadata(m_packagePath);
            if (metadata.isValid()) {
                logToFile(QString("Cache metadata matched package hash: %1").arg(metadata.packageHash));
            } else {
                logToFile("Cache hit used legacy file/mtime validation (no matching metadata record).");
            }
            if (!m_quiet && !m_json) {
                sendNotification("AppAlchemist", QString("Using cached AppImage for %1").arg(packageInfo.fileName()), "normal");
            }
            
            m_resultAppImagePath = cachedAppImage;
            m_success = true;
            
            if (m_json) {
                QJsonObject compObj;
                compObj["event"] = "complete";
                compObj["success"] = true;
                compObj["output"] = cachedAppImage;
                compObj["cached"] = true;
                compObj["dry_run"] = false;
                std::cout << QJsonDocument(compObj).toJson(QJsonDocument::Compact).toStdString() << "\n";
                std::cout.flush();
            } else if (!m_quiet) {
                std::cout << cachedAppImage.toStdString() << "\n";
                std::cout.flush();
            }
            
            if (!m_json && !m_dryRun) {
                createDesktopEntry(cachedAppImage);
                if (m_autoLaunch) {
                    if (launchAppImage(cachedAppImage)) {
                        return 0;
                    } else {
                        return 1;
                    }
                }
            }
            return 0;
        }
    }
    
    // Determine output path
    QString appImagePath = determineAppImagePath(m_packagePath, m_outputDir);
    
    // Create pipeline in separate thread
    m_pipelineThread = new QThread(this);
    m_pipeline = new PackageToAppImagePipeline();
    m_pipeline->moveToThread(m_pipelineThread);
    
    connect(m_pipelineThread, &QThread::started, m_pipeline, &PackageToAppImagePipeline::start);
    connect(m_pipeline, &PackageToAppImagePipeline::progress, this, &CliConverter::onProgress, Qt::QueuedConnection);
    connect(m_pipeline, &PackageToAppImagePipeline::log, this, &CliConverter::onLog, Qt::QueuedConnection);
    connect(m_pipeline, &PackageToAppImagePipeline::error, this, &CliConverter::onError, Qt::QueuedConnection);
    connect(m_pipeline, &PackageToAppImagePipeline::success, this, &CliConverter::onSuccess, Qt::QueuedConnection);
    connect(m_pipeline, &PackageToAppImagePipeline::finished, this, &CliConverter::onPipelineFinished, Qt::QueuedConnection);
    connect(m_pipelineThread, &QThread::finished, m_pipeline, &QObject::deleteLater);
    connect(m_pipeline, &QObject::destroyed, this, [this]() {
        m_pipeline = nullptr;
    });
    connect(m_pipelineThread, &QObject::destroyed, this, [this]() {
        m_pipelineThread = nullptr;
    });
    
    m_pipeline->setPackagePath(m_packagePath);
    m_pipeline->setOutputPath(appImagePath);
    m_pipeline->setDryRun(m_dryRun);
    
    DependencySettings dependencySettings;
    dependencySettings.bundleSystemLibraries = true;
    dependencySettings.enabled = false;
    m_pipeline->setDependencySettings(dependencySettings);
    
    OptimizationSettings optimizationSettings;
    optimizationSettings.enabled = true;
    m_pipeline->setOptimizationSettings(optimizationSettings);
    
    // Start conversion and wait for completion cleanly via QEventLoop
    QEventLoop loop;
    QMetaObject::Connection conn = connect(m_pipelineThread, &QThread::finished, &loop, &QEventLoop::quit);
    m_pipelineThread->start();
    loop.exec();
    disconnect(conn);
    
    if (m_pipelineThread && m_pipelineThread->isRunning()) {
        m_pipelineThread->wait(5000);
    }
    
    // Process remaining queued events
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    
    logToFile(QString("Pipeline finished. success=%1, autoLaunch=%2, appImagePath='%3'")
              .arg(m_success).arg(m_autoLaunch).arg(m_resultAppImagePath));
    
    if (m_success && !m_resultAppImagePath.isEmpty()) {
        qint64 elapsed = timer.elapsed();
        logToFile(QString("Conversion successful in %1 ms. autoLaunch=%2, appImagePath='%3'")
                  .arg(elapsed).arg(m_autoLaunch).arg(m_resultAppImagePath));
        
        if (!m_dryRun && !m_json) {
            createDesktopEntry(m_resultAppImagePath);
            if (m_autoLaunch) {
                logToFile("Attempting to launch AppImage...");
                QThread::msleep(500);
                if (launchAppImage(m_resultAppImagePath)) {
                    logToFile("AppImage launched successfully");
                    if (!m_quiet && elapsed < 2000) {
                        SubprocessWrapper::execute("paplay", {"/usr/share/sounds/freedesktop/stereo/complete.oga"}, {}, 1000);
                    }
                    return 0;
                } else {
                    logToFile("Failed to launch AppImage, but conversion was successful");
                    return 0;
                }
            } else {
                logToFile("Auto-launch disabled (--no-launch flag)");
            }
        }
        return 0;
    }
    
    if (!m_success) {
        logToFile("Conversion failed or was cancelled");
        if (m_json) {
            QJsonObject obj;
            obj["event"] = "complete";
            obj["success"] = false;
            obj["error"] = m_lastError.isEmpty() ? "Conversion failed" : m_lastError;
            obj["dry_run"] = m_dryRun;
            std::cout << QJsonDocument(obj).toJson(QJsonDocument::Compact).toStdString() << "\n";
            std::cout.flush();
        }
        return 1;
    }
    
    if (m_resultAppImagePath.isEmpty()) {
        logToFile("WARNING: AppImage path is empty, cannot launch");
        if (m_json) {
            QJsonObject obj;
            obj["event"] = "complete";
            obj["success"] = false;
            obj["error"] = "AppImage path is empty";
            obj["dry_run"] = m_dryRun;
            std::cout << QJsonDocument(obj).toJson(QJsonDocument::Compact).toStdString() << "\n";
            std::cout.flush();
        }
        return 1;
    }
    
    return 0;
}

int CliConverter::convert(const QString& packagePath, const QString& outputDir, bool autoLaunch,
                          bool json, bool quiet, bool dryRun) {
    CliOptions opts;
    opts.packagePath = packagePath;
    opts.outputDir = outputDir;
    opts.autoLaunch = autoLaunch;
    opts.json = json;
    opts.quiet = quiet;
    opts.dryRun = dryRun;
    return convert(opts);
}

bool CliConverter::placeCachedAppImage(const QString& cachedPath, const QString& targetPath) {
    const QFileInfo targetInfo(targetPath);
    const QDir targetDir = targetInfo.absoluteDir();
    if (!targetDir.exists() && !QDir().mkpath(targetDir.absolutePath())) {
        std::cerr << "Failed to create output directory: "
                  << targetDir.absolutePath().toStdString() << "\n";
        return false;
    }

    if (targetInfo.exists() && !QFile::remove(targetPath)) {
        std::cerr << "Failed to replace existing file: " << targetPath.toStdString() << "\n";
        return false;
    }

    if (!QFile::copy(cachedPath, targetPath)) {
        std::cerr << "Failed to copy cached AppImage to: " << targetPath.toStdString() << "\n";
        return false;
    }

    if (!SubprocessWrapper::setExecutable(targetPath)) {
        std::cerr << "Failed to mark cached AppImage executable: "
                  << targetPath.toStdString() << "\n";
        QFile::remove(targetPath);
        return false;
    }

    return true;
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

int CliConverter::convertBatch(const CliOptions& options) {
    if (options.batchPaths.isEmpty()) {
        QString errorMsg = "No packages provided for batch conversion";
        logToFile("ERROR: " + errorMsg);
        std::cerr << "Error: " << errorMsg.toStdString() << "\n";
        if (options.json) {
            QJsonObject obj;
            obj["event"] = "complete";
            obj["success"] = false;
            obj["error"] = errorMsg;
            std::cout << QJsonDocument(obj).toJson(QJsonDocument::Compact).toStdString() << "\n";
            std::cout.flush();
        }
        return 1;
    }
    
    logToFile(QString("=== Starting batch conversion of %1 packages ===").arg(options.batchPaths.size()));
    if (!options.quiet && !options.json) {
        sendNotification("AppAlchemist", QString("Starting batch conversion of %1 packages...").arg(options.batchPaths.size()), "normal");
    }
    
    int successCount = 0;
    int failCount = 0;
    QStringList successfulPaths;
    QStringList failedPaths;
    
    for (int i = 0; i < options.batchPaths.size(); ++i) {
        const QString& packagePath = options.batchPaths[i];
        QFileInfo info(packagePath);
        
        logToFile(QString("=== [%1/%2] Converting: %3 ===")
            .arg(i + 1).arg(options.batchPaths.size()).arg(info.fileName()));
        
        CliOptions itemOpts = options;
        itemOpts.packagePath = packagePath;
        itemOpts.batchPaths.clear();
        itemOpts.isBatch = false;
        itemOpts.autoLaunch = false; // Don't auto-launch during batch
        
        int result = convert(itemOpts);
        
        if (result == 0) {
            successCount++;
            if (!m_resultAppImagePath.isEmpty()) {
                successfulPaths.append(m_resultAppImagePath);
            }
            logToFile(QString("  SUCCESS: %1").arg(info.fileName()));
        } else {
            failCount++;
            failedPaths.append(info.fileName());
            logToFile(QString("  FAILED: %1").arg(info.fileName()));
        }
    }
    
    logToFile(QString("=== Batch conversion complete: %1 succeeded, %2 failed ===")
        .arg(successCount).arg(failCount));
    
    // Send summary notification
    if (!options.quiet && !options.json) {
        if (failCount == 0) {
            sendNotification("AppAlchemist", 
                QString("Batch complete: All %1 packages converted successfully").arg(successCount), 
                "normal");
        } else if (successCount == 0) {
            sendNotification("AppAlchemist Error", 
                QString("Batch failed: All %1 packages failed to convert").arg(failCount), 
                "error");
        } else {
            sendNotification("AppAlchemist", 
                QString("Batch complete: %1 succeeded, %2 failed").arg(successCount).arg(failCount), 
                "normal");
        }
    }
    
    // Launch successfully converted AppImages if requested
    if (options.autoLaunch && !successfulPaths.isEmpty() && !options.dryRun && !options.json) {
        logToFile(QString("Launching %1 converted AppImages...").arg(successfulPaths.size()));
        for (const QString& appImagePath : successfulPaths) {
            launchAppImage(appImagePath);
            QThread::msleep(500); // Small delay between launches
        }
    }
    
    return (failCount > 0) ? 1 : 0;
}

int CliConverter::convertBatch(const QStringList& packagePaths, const QString& outputDir, bool autoLaunch,
                               bool json, bool quiet, bool dryRun) {
    CliOptions opts;
    opts.batchPaths = packagePaths;
    opts.outputDir = outputDir;
    opts.autoLaunch = autoLaunch;
    opts.json = json;
    opts.quiet = quiet;
    opts.dryRun = dryRun;
    opts.isBatch = true;
    return convertBatch(opts);
}

void CliConverter::onProgress(int percentage, const QString& message) {
    QString logMsg = QString("[%1%] %2").arg(percentage).arg(message);
    logToFile(logMsg);
    if (m_json) {
        QJsonObject obj;
        obj["event"] = "progress";
        obj["percent"] = percentage;
        obj["percentage"] = percentage;
        obj["message"] = message;
        std::cout << QJsonDocument(obj).toJson(QJsonDocument::Compact).toStdString() << "\n";
        std::cout.flush();
    } else if (!m_quiet) {
        std::cerr << QString("[%1%] %2\n").arg(percentage).arg(message).toStdString();
    }
}

void CliConverter::onLog(const QString& message) {
    logToFile(message);
    if (!m_quiet && !m_json) {
        std::cerr << message.toStdString() << "\n";
    }
}

void CliConverter::onError(const QString& errorMessage) {
    m_lastError = errorMessage;
    m_success = false;
    logToFile("ERROR: " + errorMessage);
    if (!m_quiet && !m_json) {
        sendNotification("AppAlchemist Error", errorMessage, "error");
    }
    if (m_json) {
        QJsonObject obj;
        obj["event"] = "error";
        obj["message"] = errorMessage;
        std::cout << QJsonDocument(obj).toJson(QJsonDocument::Compact).toStdString() << "\n";
        std::cout.flush();
    }
    std::cerr << "Error: " << errorMessage.toStdString() << "\n";
}

void CliConverter::onSuccess(const QString& appImagePath) {
    logToFile(QString("SUCCESS: AppImage created at %1").arg(appImagePath));
    if (!m_dryRun) {
        if (CacheManager::storeConversionMetadata(m_packagePath, appImagePath)) {
            logToFile(QString("Stored conversion cache metadata for package: %1").arg(m_packagePath));
        } else {
            logToFile(QString("WARNING: Failed to store conversion cache metadata for package: %1").arg(m_packagePath));
        }
    }
    if (!m_quiet && !m_json) {
        sendNotification("AppAlchemist", QString("Successfully converted to AppImage"), "normal");
    }
    m_success = true;
    m_resultAppImagePath = appImagePath;
    logToFile(QString("onSuccess: Set m_success=true, m_resultAppImagePath='%1'").arg(appImagePath));
    if (m_json) {
        QJsonObject obj;
        obj["event"] = "complete";
        obj["success"] = true;
        obj["output"] = appImagePath;
        obj["dry_run"] = m_dryRun;
        std::cout << QJsonDocument(obj).toJson(QJsonDocument::Compact).toStdString() << "\n";
        std::cout.flush();
    } else if (!m_quiet) {
        std::cout << appImagePath.toStdString() << "\n";
        std::cout.flush();
    }
}

void CliConverter::onPipelineFinished() {
    if (m_pipelineThread) {
        m_pipelineThread->quit();
    }
}

void CliConverter::createDesktopEntry(const QString& appImagePath) {
    logToFile(QString("Creating desktop entry for: %1").arg(appImagePath));
    
    // Extract AppImage to get .desktop file
    QString tempDir = QString("/tmp/appalchemist-desktop-%1").arg(QDateTime::currentMSecsSinceEpoch());
    QDir tempDirObj(tempDir);
    if (!tempDirObj.exists()) {
        tempDirObj.mkpath(".");
    }
    
    // Extract AppImage directly without shell interpolation (SEC-CRIT-02)
    QProcess extractProcess;
    extractProcess.setWorkingDirectory(tempDir);
    extractProcess.setProgram(appImagePath);
    extractProcess.setArguments({"--appimage-extract"});
    extractProcess.start();
    if (!extractProcess.waitForFinished(30000)) {
        logToFile("WARNING: Failed to extract AppImage for desktop entry");
        tempDirObj.removeRecursively();
        return;
    }
    
    // Find .desktop file in extracted AppImage
    // First try usr/share/applications (standard location)
    QString desktopFile;
    QString desktopFileName;
    QString desktopBaseName;
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

            struct DesktopCandidate {
                QString path;
                QString fileName;
                QString baseName;
                int score = 0;
            };

            QVector<DesktopCandidate> candidates;
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
                    QString normalizedBaseName = normalizeDesktopMatchName(baseName);
                    // Skip if contains template-like patterns (numbers, texture, size indicators, etc.)
                    bool looksLikeTemplate = baseName.contains(QRegularExpression("\\d+k|\\d+bit|texture|\\d+x\\d+|template|example|sample|canon|nikon|dslr"));
                    bool looksLikeHandler = fileLower.contains("url-handler") || fileLower.contains("handler");
                    if (looksLikeHandler ||
                        fileLower.contains("template") || fileLower.contains("example") ||
                        fileLower.contains("sample") || fileLower.contains("data/") ||
                        looksLikeTemplate || !content.contains("Type=Application")) {
                        continue;
                    }

                    DesktopCandidate candidate;
                    candidate.path = filePath;
                    candidate.fileName = file;
                    candidate.baseName = fileInfo.baseName();

                    if (normalizedBaseName == expectedAppName) {
                        candidate.score += 100;
                    } else if (normalizedBaseName.contains(expectedAppName) || expectedAppName.contains(normalizedBaseName)) {
                        candidate.score += 60;
                    }

                    if (content.contains("StartupWMClass=", Qt::CaseInsensitive)) {
                        candidate.score += 20;
                    }
                    if (content.contains("Icon=", Qt::CaseInsensitive)) {
                        candidate.score += 10;
                    }
                    if (!content.contains("NoDisplay=true", Qt::CaseInsensitive)) {
                        candidate.score += 10;
                    } else {
                        candidate.score -= 120;
                    }
                    if (!content.contains("MimeType=", Qt::CaseInsensitive)) {
                        candidate.score += 5;
                    } else if (content.contains("x-scheme-handler/", Qt::CaseInsensitive)) {
                        candidate.score -= 15;
                    }

                    candidates.append(candidate);
                }
            }

            if (!candidates.isEmpty()) {
                std::sort(candidates.begin(), candidates.end(), [](const DesktopCandidate& a, const DesktopCandidate& b) {
                    if (a.score != b.score) {
                        return a.score > b.score;
                    }
                    return a.baseName.length() < b.baseName.length();
                });

                const DesktopCandidate& bestCandidate = candidates.first();
                desktopFile = bestCandidate.path;
                desktopFileName = bestCandidate.fileName;
                desktopBaseName = bestCandidate.baseName;
                logToFile(QString("Selected desktop file: %1 (score=%2)").arg(desktopFileName).arg(bestCandidate.score));
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
        QString iconName = extractAndInstallIcon(squashfsRoot, appImagePath, desktopContent, desktopBaseName);
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

    QRegularExpression noDisplayRegex("^NoDisplay\\s*=\\s*true\\s*$", QRegularExpression::MultilineOption | QRegularExpression::CaseInsensitiveOption);
    if (noDisplayRegex.match(desktopContent).hasMatch()) {
        desktopContent.replace(noDisplayRegex, "NoDisplay=false");
        logToFile("Normalized NoDisplay=true to NoDisplay=false for launcher visibility");
    }
    
    // Remove or fix Path line - it should not point to paths inside AppImage
    QRegularExpression pathRegex("^Path=(.+)$", QRegularExpression::MultilineOption);
    if (pathRegex.match(desktopContent).hasMatch()) {
        // Remove Path line as it points to non-existent system path
        desktopContent.remove(pathRegex);
        logToFile("Removed Path line from desktop entry (points to AppImage internal path)");
    }
    
    // Extract and install icon
    QString iconName = extractAndInstallIcon(squashfsRoot, appImagePath, desktopContent, desktopBaseName);
    
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
    
    QString targetDesktopFileName = desktopFileName;
    if (targetDesktopFileName.isEmpty()) {
        QString safeName = appName;
        safeName.replace(QRegularExpression("[^a-zA-Z0-9._-]"), "-");
        safeName = safeName.toLower();
        if (safeName.isEmpty()) {
            QFileInfo appImageInfo(appImagePath);
            safeName = appImageInfo.baseName().toLower();
        }
        targetDesktopFileName = QString("%1.desktop").arg(safeName);
    }

    QString targetDesktopPath = QString("%1/%2").arg(desktopDir).arg(targetDesktopFileName);
    if (desktopBaseName.isEmpty()) {
        QFileInfo appImageInfo(appImagePath);
        desktopBaseName = appImageInfo.baseName();
    }
    
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

    const QString legacyDesktopBaseName = legacySafeDesktopName(appName);
    const QString normalizedExpectedId = normalizeDesktopIdentifier(appImageInfo.baseName());
    const QString normalizedLegacyId = normalizeDesktopIdentifier(legacyDesktopBaseName);
    const QString normalizedTargetId = normalizeDesktopIdentifier(QFileInfo(targetDesktopFileName).baseName());
    const QStringList installedDesktopFiles = desktopDirObj.entryList({"*.desktop"}, QDir::Files);
    for (const QString& installedDesktopFile : installedDesktopFiles) {
        const QString installedDesktopPath = desktopDirObj.absoluteFilePath(installedDesktopFile);
        if (installedDesktopPath == targetDesktopPath) {
            continue;
        }

        const QString installedBaseName = QFileInfo(installedDesktopFile).baseName();
        const QString normalizedInstalledId = normalizeDesktopIdentifier(installedBaseName);
        const bool looksRelated = normalizedInstalledId == normalizedLegacyId ||
                                  normalizedInstalledId.contains(normalizedExpectedId) ||
                                  normalizedExpectedId.contains(normalizedInstalledId) ||
                                  normalizedInstalledId == normalizedTargetId;
        if (!looksRelated) {
            continue;
        }

        QFile installedDesktop(installedDesktopPath);
        if (!installedDesktop.open(QIODevice::ReadOnly | QIODevice::Text)) {
            continue;
        }

        const QString installedContent = installedDesktop.readAll();
        installedDesktop.close();
        if (installedContent.contains(QString("Exec=%1").arg(appImagePath))) {
            if (QFile::remove(installedDesktopPath)) {
                logToFile(QString("Removed legacy desktop entry: %1").arg(installedDesktopPath));
            } else {
                logToFile(QString("WARNING: Failed to remove legacy desktop entry: %1").arg(installedDesktopPath));
            }
        }
    }
    
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

QString CliConverter::extractAndInstallIcon(const QDir& squashfsRoot, const QString& appImagePath, const QString& desktopContent,
                                           const QString& desktopBaseName) {
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
    
    QStringList iconNamesToTry;
    for (const QString& candidate : buildIconNameVariants(iconName)) {
        if (!iconNamesToTry.contains(candidate)) {
            iconNamesToTry.append(candidate);
        }
    }
    for (const QString& candidate : buildIconNameVariants(desktopBaseName)) {
        if (!iconNamesToTry.contains(candidate)) {
            iconNamesToTry.append(candidate);
        }
    }
    for (const QString& candidate : {QString("vscodium"), QString("codium"), QString("code")}) {
        if (!iconNamesToTry.contains(candidate)) {
            iconNamesToTry.append(candidate);
        }
    }
    
    // Try to find icon in AppImage
    QStringList iconExtensions = {"png", "svg", "xpm", "ico"};
    QStringList iconSizes = {"256x256", "128x128", "64x64", "48x48", "32x32", "16x16"};
    QString foundIconPath;
    QStringList iconCandidates;

    for (const QString& tryIconName : iconNamesToTry) {
        for (const QString& size : iconSizes) {
            for (const QString& ext : iconExtensions) {
                const QString iconPath = QString("%1/usr/share/icons/hicolor/%2/apps/%3.%4")
                    .arg(squashfsRoot.absolutePath()).arg(size).arg(tryIconName).arg(ext);
                if (QFile::exists(iconPath)) {
                    iconCandidates.append(iconPath);
                }
            }
        }
    }

    for (const QString& tryIconName : iconNamesToTry) {
        for (const QString& ext : iconExtensions) {
            const QString pixmapsPath = QString("%1/usr/share/pixmaps/%2.%3")
                .arg(squashfsRoot.absolutePath()).arg(tryIconName).arg(ext);
            if (QFile::exists(pixmapsPath)) {
                iconCandidates.append(pixmapsPath);
            }

            const QString rootPath = QString("%1/%2.%3")
                .arg(squashfsRoot.absolutePath()).arg(tryIconName).arg(ext);
            if (QFile::exists(rootPath)) {
                iconCandidates.append(rootPath);
            }
        }
    }

    for (const QString& ext : iconExtensions) {
        QDirIterator it(squashfsRoot.absolutePath(),
                        {QString("*.%1").arg(ext)},
                        QDir::Files,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString candidatePath = it.next();
            const QString lowerPath = candidatePath.toLower();

            // Skip node_modules, __pycache__, and other non-icon directories
            if (lowerPath.contains("/node_modules/") || lowerPath.contains("/__pycache__/") ||
                lowerPath.contains("/test/") || lowerPath.contains("/tests/") ||
                lowerPath.contains("/.git/")) {
                continue;
            }

            bool matchesPreferredName = false;
            for (const QString& tryIconName : iconNamesToTry) {
                if (!tryIconName.isEmpty() && lowerPath.contains(tryIconName.toLower())) {
                    matchesPreferredName = true;
                    break;
                }
            }

            if (matchesPreferredName || lowerPath.contains("product_logo") || lowerPath.contains("/icons/")) {
                iconCandidates.append(candidatePath);
            }
        }
    }

    foundIconPath = selectBestIconAsset(iconCandidates, iconNamesToTry);
    if (!foundIconPath.isEmpty()) {
        const QFileInfo selectedInfo(foundIconPath);
        iconName = selectedInfo.baseName();
        logToFile(QString("Selected best icon: %1").arg(foundIconPath));
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
                if (!looksLikeTemplate && scoreIconAsset(potentialIconPath) > 1) {
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
                if (!looksLikeTemplate && scoreIconAsset(potentialIconPath) > 1) {
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
            if (copyFileReplacing(foundIconPath, targetPath)) {
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
        if (copyFileReplacing(foundIconPath, targetPath)) {
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
                    copyFileReplacing(foundIconPath, targetPathSize);
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
    
    // Launch AppImage directly without shell interpolation (SEC-CRIT-02)
    bool started = QProcess::startDetached(absolutePath, QStringList());
    if (started) {
        logToFile("AppImage launched successfully using QProcess::startDetached");
        sendNotification("AppAlchemist", QString("Launching %1...").arg(QFileInfo(appImagePath).fileName()), "normal");
        return true;
    }

    logToFile("Direct QProcess::startDetached failed, trying SubprocessWrapper...");
    const ProcessResult res = SubprocessWrapper::execute(absolutePath, {}, {}, 10000);
    if (res.exitCode == 0) {
        logToFile("AppImage launched successfully using SubprocessWrapper");
        sendNotification("AppAlchemist", QString("Launching %1...").arg(QFileInfo(appImagePath).fileName()), "normal");
        return true;
    }

    // All methods failed
    logToFile(QString("All launch methods failed for %1. Last error code: %2").arg(absolutePath).arg(res.exitCode));
    sendNotification("AppAlchemist Error", QString("Failed to launch %1").arg(QFileInfo(appImagePath).fileName()), "error");
    return false;
}
