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
};

#endif // APPDIRBUILDER_H
