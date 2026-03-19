#ifndef TARBALLPARSER_H
#define TARBALLPARSER_H

#include "debparser.h"  // For PackageMetadata
#include <QString>
#include <QStringList>

// Tarball compression types
enum class TarballType {
    TAR_GZ,     // .tar.gz, .tgz
    TAR_XZ,     // .tar.xz, .txz
    TAR_BZ2,    // .tar.bz2, .tbz2
    TAR_ZSTD,   // .tar.zst
    ZIP,        // .zip
    TAR,        // .tar (uncompressed)
    UNKNOWN
};

// Structure type - how files are organized inside the archive
enum class TarballStructure {
    STANDARD,       // Has usr/, opt/ structure (like a package)
    FLAT,           // All files in root or single directory
    ELECTRON,       // Electron app structure (has resources/, locales/, etc.)
    APPDIR          // Already an AppDir structure
};

class TarballParser {
public:
    TarballParser();
    ~TarballParser();
    
    // Validate tarball file
    bool validateTarball(const QString& tarballPath);
    
    // Extract tarball to directory
    bool extractTarball(const QString& tarballPath, const QString& extractDir);
    
    // Parse metadata from extracted files
    PackageMetadata parseMetadata(const QString& extractDir);
    
    // Get tarball type from file path
    static TarballType getTarballType(const QString& tarballPath);
    
    // Check if file is a supported tarball
    static bool isSupportedTarball(const QString& filePath);
    
    // Get file extension filter for file dialogs
    static QString getFileFilter();

private:
    QString m_tempDir;
    TarballType m_type;
    TarballStructure m_structure;
    
    // Detection methods
    TarballStructure detectStructure(const QString& extractDir);
    bool isElectronApp(const QString& dirPath);
    
    // Extraction methods
    bool extractTar(const QString& tarPath, const QString& extractDir, const QString& compression);
    bool extractZip(const QString& zipPath, const QString& extractDir);
    
    // Metadata extraction
    QString findDesktopFile(const QString& extractDir);
    QString parseDesktopFile(const QString& desktopPath, PackageMetadata& metadata);
    QStringList findExecutables(const QString& extractDir);
    QString findIcon(const QString& extractDir);
    QString guessPackageName(const QString& tarballPath, const QString& extractDir);
    QString guessVersion(const QString& tarballPath, const QString& extractDir);
    
    // Helper methods
    QStringList searchInDirectory(const QString& dir, const QStringList& patterns, bool executableOnly = false);
    bool isElfExecutable(const QString& filePath);
    bool isScript(const QString& filePath);
    QString findMainDirectory(const QString& extractDir);
};

#endif // TARBALLPARSER_H


