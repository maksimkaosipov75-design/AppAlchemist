#include "package_packager.h"
#include "appdirbuilder.h"
#include "appimagebuilder.h"
#include <QDir>
#include <QFileInfo>

PackagePackager::PackagePackager(AppDirBuilder* appDirBuilder,
                                 AppImageBuilder* appImageBuilder)
    : m_appDirBuilder(appDirBuilder)
    , m_appImageBuilder(appImageBuilder) {
}

AppDirBuildResult PackagePackager::buildAppDir(const QString& tempDir,
                                               const QString& extractedDir,
                                               const PackageMetadata& metadata,
                                               const QStringList& libraries) const {
    AppDirBuildResult result;
    result.appDirPath = QString("%1/AppDir").arg(tempDir);
    result.logs << QString("Building AppDir at: %1").arg(result.appDirPath);
    result.logs << QString("Copying %1 executable(s)").arg(metadata.executables.size());
    result.logs << QString("Copying %1 library(ies)").arg(libraries.size());

    if (!m_appDirBuilder->buildAppDir(result.appDirPath, extractedDir, metadata, libraries)) {
        result.error = "Failed to build AppDir";
        result.logs << "ERROR: Failed to build AppDir";
        result.logs << "Check the logs above for specific error details";
        return result;
    }

    result.logs << "Successfully built AppDir structure";

    const QString desktopDir = QString("%1/usr/share/applications").arg(result.appDirPath);
    const QDir desktopDirObj(desktopDir);
    if (desktopDirObj.exists()) {
        const QStringList desktopFiles = desktopDirObj.entryList({"*.desktop"}, QDir::Files);
        if (desktopFiles.isEmpty()) {
            result.logs << "WARNING: No .desktop files found after building AppDir!";
            result.logs << QString("Checked directory: %1").arg(desktopDir);
        } else {
            result.logs << QString("Verified .desktop file(s) exist: %1").arg(desktopFiles.join(", "));
            for (const QString& file : desktopFiles) {
                const QString fullPath = desktopDirObj.absoluteFilePath(file);
                const QFileInfo info(fullPath);
                result.logs << QString("  - %1 (size: %2 bytes)").arg(file).arg(info.size());
            }
        }
    } else {
        result.error = QString("Desktop directory does not exist: %1").arg(desktopDir);
        result.logs << QString("ERROR: Desktop directory does not exist: %1").arg(desktopDir);
        return result;
    }

    result.success = true;
    return result;
}

AppImagePackagingResult PackagePackager::buildAppImage(const QString& packagePath,
                                                       const QString& appDirPath,
                                                       const QString& requestedOutputPath) const {
    AppImagePackagingResult result;
    result.outputPath = requestedOutputPath;
    if (result.outputPath.isEmpty()) {
        const QFileInfo packageInfo(packagePath);
        const QString baseName = packageInfo.baseName();
        result.outputPath = QString("%1/%2.AppImage").arg(QDir::homePath()).arg(baseName);
    }

    result.logs << QString("Building AppImage: %1").arg(result.outputPath);
    if (!m_appImageBuilder->buildAppImage(appDirPath, result.outputPath)) {
        result.error = "Failed to build AppImage.";
        result.logs << "ERROR: Failed to build AppImage.";
        return result;
    }

    result.logs << QString("Successfully created AppImage: %1").arg(result.outputPath);
    result.success = true;
    return result;
}
