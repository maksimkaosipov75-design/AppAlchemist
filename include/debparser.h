#ifndef DEBPARSER_H
#define DEBPARSER_H

#include <QString>
#include <QStringList>
#include <QMap>

// Common metadata structure for both DEB and RPM packages
struct PackageMetadata {
    QString package;
    QString version;
    QString description;
    QStringList depends;
    QStringList executables;
    QStringList scripts;  // postinst, prerm, etc.
    QString iconPath;
    QString mainExecutable;
    QString desktopExecCommand;
    QString desktopFileContent; // For Java apps
    QString javaMainClass;      // For Java apps
    QString javaJarPath;        // For Java apps
};

// Alias for backward compatibility
using DebMetadata = PackageMetadata;

class DebParser {
public:
    DebParser();
    ~DebParser();
    
    bool validateDebFile(const QString& debPath);
    bool extractDeb(const QString& debPath, const QString& extractDir);
    DebMetadata parseMetadata(const QString& extractDir);
    QStringList findExecutables(const QString& extractDir);
    QStringList findScripts(const QString& extractDir);
    QString findIcon(const QString& extractDir);
    QString findDesktopFile(const QString& extractDir);
    QString parseDesktopFile(const QString& desktopPath, DebMetadata& metadata);
    
private:
    QString m_tempDir;
    bool parseControlFile(const QString& controlPath, DebMetadata& metadata);
    QStringList searchInDirectory(const QString& dir, const QStringList& patterns, bool executableOnly = false);
    bool isElfExecutable(const QString& filePath);
};

#endif // DEBPARSER_H
