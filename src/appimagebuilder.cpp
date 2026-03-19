#include "appimagebuilder.h"
#include "utils.h"
#include <QFileInfo>
#include <QStandardPaths>
#include <QDir>
#include <QDebug>
#include <QCoreApplication>
#include <QFile>
#include <QDateTime>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QEventLoop>
#include <QUrl>

AppImageBuilder::AppImageBuilder(QObject* parent)
    : QObject(parent)
{
    // Preload appimagetool on first construction
    static bool preloaded = false;
    if (!preloaded) {
        preloadAppImageTool();
        preloaded = true;
    }
    
    m_appImageToolPath = findAppImageTool();
}

QString AppImageBuilder::getCachedAppImageToolPath() {
    QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QString appAlchemistCache = QString("%1/appalchemist").arg(cacheDir);
    QDir cacheDirObj;
    if (!cacheDirObj.exists(appAlchemistCache)) {
        cacheDirObj.mkpath(appAlchemistCache);
    }
    return QString("%1/appimagetool").arg(appAlchemistCache);
}

bool AppImageBuilder::cacheAppImageTool(const QString& sourcePath) {
    QString cachedPath = getCachedAppImageToolPath();
    QFileInfo sourceInfo(sourcePath);
    QFileInfo cachedInfo(cachedPath);
    
    // Check if cached version exists and is newer
    if (cachedInfo.exists() && cachedInfo.lastModified() >= sourceInfo.lastModified()) {
        return true; // Already cached and up to date
    }
    
    // Copy to cache
    if (QFile::copy(sourcePath, cachedPath)) {
        QFile::setPermissions(cachedPath, QFile::ExeUser | QFile::ExeGroup | QFile::ExeOther | QFile::ReadUser | QFile::ReadGroup | QFile::ReadOther);
        return true;
    }
    
    return false;
}

void AppImageBuilder::preloadAppImageTool() {
    QString toolPath = findAppImageTool();
    if (!toolPath.isEmpty()) {
        cacheAppImageTool(toolPath);
        qDebug() << "AppImageTool preloaded and cached:" << getCachedAppImageToolPath();
    }
}

QString AppImageBuilder::getBundledAppImageToolPath() {
    // Check for bundled appimagetool in application directory
    QString appDir = QCoreApplication::applicationDirPath();
    QString bundledPath = QString("%1/usr/bin/appimagetool").arg(appDir);
    if (QFileInfo::exists(bundledPath) && checkAppImageTool(bundledPath)) {
        return bundledPath;
    }
    // Also check in usr/lib/appalchemist/appimagetool
    bundledPath = QString("%1/usr/lib/appalchemist/appimagetool").arg(appDir);
    if (QFileInfo::exists(bundledPath) && checkAppImageTool(bundledPath)) {
        return bundledPath;
    }
    return QString();
}

QString AppImageBuilder::findAppImageTool() {
    // First check bundled appimagetool (for AppImage/installed versions)
    QString bundledPath = getBundledAppImageToolPath();
    if (!bundledPath.isEmpty()) {
        qDebug() << "Found bundled appimagetool at:" << bundledPath;
        cacheAppImageTool(bundledPath);
        return bundledPath;
    }
    
    // Then check cache
    QString cachedPath = getCachedAppImageToolPath();
    if (QFileInfo::exists(cachedPath) && checkAppImageTool(cachedPath)) {
        return cachedPath;
    }
    
    // Check in application directory
    QString appDir = QCoreApplication::applicationDirPath();
    // Get user's local bin directory
    QString homeDir = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    QString localBin = QString("%1/.local/bin/appimagetool").arg(homeDir);
    
    QStringList searchPaths = {
        QString("%1/appimagetool").arg(appDir),
        QString("%1/thirdparty/appimagetool").arg(appDir),
        QString("%1/../thirdparty/appimagetool").arg(appDir),
        QString("%1/../../thirdparty/appimagetool").arg(appDir),  // For build directory
        localBin,  // User's local bin directory
        "/usr/bin/appimagetool",
        "/usr/local/bin/appimagetool"
    };
    
    // Also check in project root directory (where CMakeLists.txt is)
    QDir projectRoot = QDir(QCoreApplication::applicationDirPath());
    // If we're in build directory, go up one level
    if (projectRoot.dirName() == "build") {
        projectRoot.cdUp();
    }
    searchPaths.append(projectRoot.absoluteFilePath("thirdparty/appimagetool"));
    searchPaths.append(projectRoot.absoluteFilePath("appimagetool"));
    
    // Also check in current directory
    QDir currentDir = QDir::current();
    searchPaths.append(currentDir.absoluteFilePath("appimagetool"));
    searchPaths.append(currentDir.absoluteFilePath("thirdparty/appimagetool"));
    
    qDebug() << "Searching for appimagetool in:" << searchPaths;
    
    for (const QString& path : searchPaths) {
        if (checkAppImageTool(path)) {
            qDebug() << "Found appimagetool at:" << path;
            // Cache it for future use
            cacheAppImageTool(path);
            return path;
        }
    }
    
    // Try to download appimagetool if not found
    qWarning() << "appimagetool not found in any of the search paths, attempting to download...";
    if (downloadAppImageTool()) {
        cachedPath = getCachedAppImageToolPath();
        if (checkAppImageTool(cachedPath)) {
            return cachedPath;
        }
    }
    
    qWarning() << "appimagetool not found and download failed";
    return QString();
}

bool AppImageBuilder::checkAppImageTool(const QString& path) {
    QFileInfo info(path);
    if (!info.exists() || !info.isExecutable()) {
        return false;
    }
    
    // Try to run it with --version
    ProcessResult result = SubprocessWrapper::execute(path, {"--version"}, {}, 5000);
    return result.success;
}

bool AppImageBuilder::downloadAppImageTool() {
    QString cachedPath = getCachedAppImageToolPath();
    QFileInfo cachedInfo(cachedPath);
    
    // If already cached and valid, don't download again
    if (cachedInfo.exists() && checkAppImageTool(cachedPath)) {
        return true;
    }
    
    qDebug() << "Downloading appimagetool from GitHub...";
    
    // Download appimagetool from GitHub releases
    // Detect architecture and use appropriate version
    QString arch = detectSystemArchitecture();
    QString downloadUrl;
    if (arch == "aarch64") {
        downloadUrl = "https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-aarch64.AppImage";
    } else {
        downloadUrl = "https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage";
    }
    
    QNetworkAccessManager manager;
    QUrl url(downloadUrl);
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "AppAlchemist/1.0.0");
    
    QEventLoop loop;
    QNetworkReply *reply = manager.get(request);
    
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    
    if (reply->error() != QNetworkReply::NoError) {
        qWarning() << "Failed to download appimagetool:" << reply->errorString();
        reply->deleteLater();
        return false;
    }
    
    // Save to cache
    QDir cacheDir = QFileInfo(cachedPath).dir();
    if (!cacheDir.exists()) {
        cacheDir.mkpath(".");
    }
    
    QFile file(cachedPath);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "Failed to open cache file for writing:" << cachedPath;
        reply->deleteLater();
        return false;
    }
    
    file.write(reply->readAll());
    file.close();
    
    // Make executable
    QFile::setPermissions(cachedPath, QFile::ExeUser | QFile::ExeGroup | QFile::ExeOther | QFile::ReadUser | QFile::ReadGroup | QFile::ReadOther);
    
    reply->deleteLater();
    
    qDebug() << "Successfully downloaded appimagetool to:" << cachedPath;
    return checkAppImageTool(cachedPath);
}

bool AppImageBuilder::buildAppImage(const QString& appDirPath, const QString& outputPath, bool compress) {
    if (m_appImageToolPath.isEmpty()) {
        qWarning() << "AppImageTool not found!";
        qWarning() << "Please download appimagetool from:";
        qWarning() << "https://github.com/AppImage/AppImageKit/releases";
        qWarning() << "And place it in one of these locations:";
        qWarning() << "  - thirdparty/appimagetool (in project directory)";
        qWarning() << "  - /usr/bin/appimagetool (system-wide)";
        qWarning() << "  - /usr/local/bin/appimagetool (local)";
        return false;
    }
    
    QFileInfo appDirInfo(appDirPath);
    if (!appDirInfo.exists() || !appDirInfo.isDir()) {
        emit log(QString("ERROR: AppDir does not exist: %1").arg(appDirPath));
        return false;
    }
    
    // Check for .desktop file
    QString desktopDir = QString("%1/usr/share/applications").arg(appDirPath);
    QDir desktopDirObj(desktopDir);
    QStringList desktopFiles = desktopDirObj.entryList({"*.desktop"}, QDir::Files);
    
    if (desktopFiles.isEmpty()) {
        emit log("ERROR: No .desktop file found in AppDir/usr/share/applications/");
        emit log(QString("Checked directory: %1").arg(desktopDir));
        return false;
    }
    
    emit log(QString("Found .desktop file(s): %1").arg(desktopFiles.join(", ")));
    
    // Ensure output directory exists
    QFileInfo outputInfo(outputPath);
    QDir outputDir = outputInfo.dir();
    if (!outputDir.exists()) {
        if (!outputDir.mkpath(".")) {
            emit log(QString("ERROR: Failed to create output directory: %1").arg(outputDir.absolutePath()));
            return false;
        }
    }
    
    // Build AppImage
    emit log(QString("Running appimagetool: %1").arg(m_appImageToolPath));
    emit log(QString("AppDir: %1").arg(appDirPath));
    emit log(QString("Output: %1").arg(outputPath));
    
    // Set ARCH environment variable (required by appimagetool)
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    QString arch = detectSystemArchitecture();
    env.insert("ARCH", arch);
    
    // Build arguments - add -n flag if compression is disabled
    QStringList args;
    if (!compress) {
        args << "-n";
        emit log("Building AppImage without compression (faster)");
    }

    // Add --no-appstream flag to skip strict AppStream validation
    // Many packages have invalid AppStream metadata that prevents AppImage creation
    args << "--no-appstream";

    args << appDirPath << outputPath;
    
    ProcessResult result = SubprocessWrapper::execute(
        m_appImageToolPath,
        args,
        {},
        300000,  // 5 minute timeout
        env
    );
    
    // Log output
    if (!result.stdoutOutput.isEmpty()) {
        QStringList lines = result.stdoutOutput.split('\n', Qt::SkipEmptyParts);
        for (const QString& line : lines) {
            emit log(line);
        }
    }
    
    if (!result.success) {
        emit log(QString("ERROR: AppImageTool failed with exit code: %1").arg(result.exitCode));
        emit log(QString("Error: %1").arg(result.errorMessage));
        if (!result.stderrOutput.isEmpty()) {
            QStringList errorLines = result.stderrOutput.split('\n', Qt::SkipEmptyParts);
            for (const QString& line : errorLines) {
                emit log(QString("stderr: %1").arg(line));
            }
        }
        return false;
    }
    
    emit log("AppImageTool completed successfully");
    
    // Make AppImage executable
    if (QFileInfo::exists(outputPath)) {
        SubprocessWrapper::setExecutable(outputPath);
        return true;
    }
    
    return false;
}

