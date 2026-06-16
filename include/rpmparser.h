#ifndef RPMPARSER_H
#define RPMPARSER_H

#include "debparser.h"  // For PackageMetadata
#include <QString>
#include <QStringList>
#include <QMap>

// Metadata read directly from the binary RPM header (lead + main header).
// This is parsed in-process and does NOT require the external `rpm` binary,
// giving RPM the same self-contained metadata access that DEB has via its
// control file.
struct RpmHeaderInfo {
    QString name;
    QString version;
    QString release;
    QString summary;
    QStringList requires_;     // raw RPMTAG_REQUIRENAME entries
    QString payloadFormat;     // e.g. "cpio"
    QString payloadCompressor; // e.g. "zstd", "xz", "gzip", "lzma", "bzip2"
    bool valid = false;
};

class RpmParser {
public:
    RpmParser();
    ~RpmParser();

    bool validateRpmFile(const QString& rpmPath);
    bool extractRpm(const QString& rpmPath, const QString& extractDir);
    PackageMetadata parseMetadata(const QString& extractDir, const QString& packageName = QString());

    // Parse the RPM header without any external tools. Returns valid=false on
    // malformed input. Safe to call before extraction.
    static RpmHeaderInfo readRpmHeader(const QString& rpmPath);

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

