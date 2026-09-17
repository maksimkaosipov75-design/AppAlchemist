#ifndef APPDIRBUILDER_H
#define APPDIRBUILDER_H

#include "debparser.h"
#include <QString>
#include <QStringList>
#include <QTextStream>

class AppDirBuilder {
public:
    AppDirBuilder();
    
    bool buildAppDir(const QString& appDirPath,
                     const QString& extractedDebDir,
                     const PackageMetadata& metadata,
                     const QStringList& libraries);

    bool createDesktopFile(const QString& appDirPath, const PackageMetadata& metadata);
    bool fixDesktopFile(const QString& desktopPath, const PackageMetadata& metadata);
    bool copyIcon(const QString& appDirPath, const QString& iconPath, const PackageMetadata& metadata);
    bool createAppRun(const QString& appDirPath, const PackageMetadata& metadata);

    // Search for appimagetool across PATH, AppImage runtimes, standard system paths, and custom env var (REL-LOW-52)
    static QString findAppImageTool();

    // Rewrites references to the directories the package installs under its
    // own name (/usr/share/<name>, /opt/<name>, ...) so they resolve inside
    // the bundle. Applications that compile their data prefix in otherwise
    // read those paths on the host, where they do not exist, and fail to
    // start. Returns the absolute paths that were relocated.
    QStringList relocatePackagePaths(const QString& appDirPath, const PackageMetadata& metadata);
    
private:
    bool createDirectoryStructure(const QString& appDirPath);
    bool copyExecutables(const QString& appDirPath, 
                        const QString& extractedDebDir,
                        const QStringList& executables);
    bool copyLibraries(const QString& appDirPath, const QStringList& libraries);
    bool copyResources(const QString& appDirPath, const QString& extractedDebDir);
    bool copyMissingDirectoryContents(const QString& sourcePath, const QString& targetPath);
    // Exports the environment a bundled GLib/GTK stack needs to find its own
    // loadable modules, for whichever of those trees are present in the AppDir.
    void writeRuntimeModuleEnvironment(QTextStream& out, const QString& appDirPath);

    // Set when relocatePackagePaths() rewrote something: the generated AppRun
    // then has to run the application from the bundle's usr directory, which
    // is what the rewritten relative paths resolve against.
    bool m_relocatedPackagePaths = false;
};

#endif // APPDIRBUILDER_H
