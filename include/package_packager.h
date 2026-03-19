#ifndef PACKAGE_PACKAGER_H
#define PACKAGE_PACKAGER_H

#include "debparser.h"
#include <QString>
#include <QStringList>

class AppDirBuilder;
class AppImageBuilder;

struct AppDirBuildResult {
    bool success = false;
    QString error;
    QString appDirPath;
    QStringList logs;
};

struct AppImagePackagingResult {
    bool success = false;
    QString error;
    QString outputPath;
    QStringList logs;
};

class PackagePackager {
public:
    PackagePackager(AppDirBuilder* appDirBuilder,
                    AppImageBuilder* appImageBuilder);

    AppDirBuildResult buildAppDir(const QString& tempDir,
                                  const QString& extractedDir,
                                  const PackageMetadata& metadata,
                                  const QStringList& libraries) const;

    AppImagePackagingResult buildAppImage(const QString& packagePath,
                                          const QString& appDirPath,
                                          const QString& requestedOutputPath) const;

private:
    AppDirBuilder* m_appDirBuilder;
    AppImageBuilder* m_appImageBuilder;
};

#endif // PACKAGE_PACKAGER_H
