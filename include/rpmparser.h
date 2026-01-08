#ifndef RPMPARSER_H
#define RPMPARSER_H

#include "debparser.h"  // For PackageMetadata
#include <QString>
#include <QStringList>
#include <QMap>

class RpmParser {
public:
    RpmParser();
    ~RpmParser();
    
    bool validateRpmFile(const QString& rpmPath);
    bool extractRpm(const QString& rpmPath, const QString& extractDir);
    PackageMetadata parseMetadata(const QString& extractDir);
    
private:
    QString m_tempDir;
    bool parseSpecFile(const QString& specPath, PackageMetadata& metadata);
    QStringList findExecutables(const QString& extractDir);
    QStringList findScripts(const QString& extractDir);
    QString findIcon(const QString& extractDir);
    QString findDesktopFile(const QString& extractDir);
    QString parseDesktopFile(const QString& desktopPath, PackageMetadata& metadata);
    QStringList searchInDirectory(const QString& dir, const QStringList& patterns, bool executableOnly = false);
    bool isElfExecutable(const QString& filePath);
    bool isScriptFile(const QString& filePath);
    QStringList findJavaApplications(const QString& extractDir);
};

#endif // RPMPARSER_H

