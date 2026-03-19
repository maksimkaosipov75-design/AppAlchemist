#ifndef APPDIRBUILDER_H
#define APPDIRBUILDER_H

#include "debparser.h"
#include <QString>
#include <QStringList>

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
};

#endif // APPDIRBUILDER_H
