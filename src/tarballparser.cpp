#include "tarballparser.h"
#include "utils.h"
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QTextStream>
#include <QStandardPaths>
#include <QDebug>
#include <QRegularExpression>
#include <QDateTime>
#include <QDirIterator>

TarballParser::TarballParser()
    : m_type(TarballType::UNKNOWN)
    , m_structure(TarballStructure::FLAT)
{
    QString tempBase = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    m_tempDir = QString("%1/tarball-parser-%2").arg(tempBase).arg(QString::number(QDateTime::currentMSecsSinceEpoch()));
}

TarballParser::~TarballParser() {
    if (!m_tempDir.isEmpty() && QDir(m_tempDir).exists()) {
        SubprocessWrapper::removeDirectory(m_tempDir);
    }
}

TarballType TarballParser::getTarballType(const QString& tarballPath) {
    QString lower = tarballPath.toLower();
    
    if (lower.endsWith(".tar.gz") || lower.endsWith(".tgz")) {
        return TarballType::TAR_GZ;
    } else if (lower.endsWith(".tar.xz") || lower.endsWith(".txz")) {
        return TarballType::TAR_XZ;
    } else if (lower.endsWith(".tar.bz2") || lower.endsWith(".tbz2") || lower.endsWith(".tbz")) {
        return TarballType::TAR_BZ2;
    } else if (lower.endsWith(".tar.zst") || lower.endsWith(".tzst")) {
        return TarballType::TAR_ZSTD;
    } else if (lower.endsWith(".tar.bin")) {
        return TarballType::TAR_BIN;
    } else if (lower.endsWith(".zip")) {
        return TarballType::ZIP;
    } else if (lower.endsWith(".tar")) {
        return TarballType::TAR;
    }
    
    return TarballType::UNKNOWN;
}

bool TarballParser::isSupportedTarball(const QString& filePath) {
    return getTarballType(filePath) != TarballType::UNKNOWN;
}

QString TarballParser::getFileFilter() {
    return "Archives (*.tar.gz *.tgz *.tar.xz *.txz *.tar.bz2 *.tbz2 *.tar.zst *.tar.bin *.zip *.tar);;"
           "Gzip archives (*.tar.gz *.tgz);;"
           "XZ archives (*.tar.xz *.txz);;"
           "Bzip2 archives (*.tar.bz2 *.tbz2);;"
           "Zstd archives (*.tar.zst);;"
           "Tar bin archives (*.tar.bin);;"
           "ZIP archives (*.zip);;"
           "Tar archives (*.tar)";
}

bool TarballParser::validateTarball(const QString& tarballPath) {
    QFileInfo info(tarballPath);
    if (!info.exists() || !info.isFile()) {
        return false;
    }
    
    m_type = getTarballType(tarballPath);
    m_sourcePath = tarballPath;
    if (m_type == TarballType::UNKNOWN) {
        return false;
    }
    
    // Check magic bytes
    QFile file(tarballPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    
    QByteArray header = file.read(8);
    file.close();
    
    switch (m_type) {
        case TarballType::TAR_GZ:
            // Gzip magic: 0x1F 0x8B
            return header.size() >= 2 && 
                   static_cast<unsigned char>(header[0]) == 0x1F && 
                   static_cast<unsigned char>(header[1]) == 0x8B;
        
        case TarballType::TAR_XZ:
            // XZ magic: 0xFD '7' 'z' 'X' 'Z' 0x00
            return header.size() >= 6 && 
                   static_cast<unsigned char>(header[0]) == 0xFD &&
                   header[1] == '7' && header[2] == 'z' && 
                   header[3] == 'X' && header[4] == 'Z' &&
                   static_cast<unsigned char>(header[5]) == 0x00;
        
        case TarballType::TAR_BZ2:
            // Bzip2 magic: 'B' 'Z' 'h'
            return header.size() >= 3 && 
                   header[0] == 'B' && header[1] == 'Z' && header[2] == 'h';
        
        case TarballType::TAR_ZSTD:
            // Zstd magic: 0x28 0xB5 0x2F 0xFD
            return header.size() >= 4 && 
                   static_cast<unsigned char>(header[0]) == 0x28 &&
                   static_cast<unsigned char>(header[1]) == 0xB5 &&
                   static_cast<unsigned char>(header[2]) == 0x2F &&
                   static_cast<unsigned char>(header[3]) == 0xFD;
        
        case TarballType::ZIP:
            // ZIP magic: 'P' 'K' 0x03 0x04 or 'P' 'K' 0x05 0x06 (empty) or 'P' 'K' 0x07 0x08 (spanned)
            return header.size() >= 4 && header[0] == 'P' && header[1] == 'K';

        case TarballType::TAR_BIN:
            return true;
        
        case TarballType::TAR:
            // Tar files have magic "ustar" at offset 257, but checking is complex
            // Accept if extension matches
            return true;
        
        default:
            return false;
    }
}

bool TarballParser::extractTar(const QString& tarPath, const QString& extractDir, const QString& compression) {
    QStringList args;
    
    if (compression == "gz") {
        args = {"-xzf", tarPath, "-C", extractDir};
    } else if (compression == "xz") {
        args = {"-xJf", tarPath, "-C", extractDir};
    } else if (compression == "bz2") {
        args = {"-xjf", tarPath, "-C", extractDir};
    } else if (compression == "zstd") {
        args = {"--zstd", "-xf", tarPath, "-C", extractDir};
    } else {
        args = {"-xf", tarPath, "-C", extractDir};
    }
    
    ProcessResult result = SubprocessWrapper::execute("tar", args);
    return result.success;
}

bool TarballParser::extractZip(const QString& zipPath, const QString& extractDir) {
    // Try unzip first
    ProcessResult result = SubprocessWrapper::execute("unzip", {"-q", "-o", zipPath, "-d", extractDir});
    if (result.success) {
        return true;
    }
    
    // Fallback to 7z
    result = SubprocessWrapper::execute("7z", {"x", "-y", QString("-o%1").arg(extractDir), zipPath});
    if (result.success) {
        return true;
    }
    
    // Fallback to bsdtar
    result = SubprocessWrapper::execute("bsdtar", {"-xf", zipPath, "-C", extractDir});
    return result.success;
}

bool TarballParser::extractTarball(const QString& tarballPath, const QString& extractDir) {
    if (!validateTarball(tarballPath)) {
        return false;
    }
    
    // Create extraction directory
    if (!SubprocessWrapper::createDirectory(extractDir)) {
        return false;
    }
    
    QString dataDir = QString("%1/data").arg(extractDir);
    if (!SubprocessWrapper::createDirectory(dataDir)) {
        return false;
    }
    
    bool success = false;
    
    switch (m_type) {
        case TarballType::TAR_GZ:
            success = extractTar(tarballPath, dataDir, "gz");
            break;
        case TarballType::TAR_XZ:
            success = extractTar(tarballPath, dataDir, "xz");
            break;
        case TarballType::TAR_BZ2:
            success = extractTar(tarballPath, dataDir, "bz2");
            break;
        case TarballType::TAR_ZSTD:
            success = extractTar(tarballPath, dataDir, "zstd");
            break;
        case TarballType::TAR_BIN:
            success = extractTar(tarballPath, dataDir, "");
            break;
        case TarballType::ZIP:
            success = extractZip(tarballPath, dataDir);
            break;
        case TarballType::TAR:
            success = extractTar(tarballPath, dataDir, "");
            break;
        default:
            return false;
    }
    
    if (!success) {
        return false;
    }
    
    // Detect structure
    m_structure = detectStructure(dataDir);
    
    return true;
}

QString TarballParser::findMainDirectory(const QString& extractDir) {
    QDir dir(extractDir);
    QStringList entries = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    
    // If there's only one directory, it's likely the main directory
    if (entries.size() == 1) {
        return dir.absoluteFilePath(entries.first());
    }
    
    // Check for standard directories
    for (const QString& entry : entries) {
        if (entry == "usr" || entry == "opt" || entry == "bin") {
            return extractDir;  // Standard structure at root
        }
    }
    
    // If multiple directories but one looks like app name, use it
    if (entries.size() == 1) {
        return dir.absoluteFilePath(entries.first());
    }
    
    return extractDir;
}

TarballStructure TarballParser::detectStructure(const QString& extractDir) {
    QString mainDir = findMainDirectory(extractDir);
    QDir dir(mainDir);
    
    // Check for standard package structure
    if (dir.exists("usr") || dir.exists("opt")) {
        return TarballStructure::STANDARD;
    }
    
    // Check for AppDir structure
    if (dir.exists("AppRun") || dir.exists("usr/bin")) {
        return TarballStructure::APPDIR;
    }
    
    // Check for Electron app structure
    if (isElectronApp(mainDir)) {
        return TarballStructure::ELECTRON;
    }
    
    return TarballStructure::FLAT;
}

bool TarballParser::isElectronApp(const QString& dirPath) {
    QDir dir(dirPath);
    
    // Electron apps have specific structure
    if (dir.exists("resources") && 
        (dir.exists("locales") || dir.exists("chrome_100_percent.pak"))) {
        return true;
    }
    
    // Check for .asar files
    QDirIterator it(dirPath, {"*.asar"}, QDir::Files, QDirIterator::Subdirectories);
    if (it.hasNext()) {
        return true;
    }
    
    // Check for electron binary
    QStringList electronBinaries = {"electron", "chrome-sandbox", "libffmpeg.so"};
    for (const QString& binary : electronBinaries) {
        if (dir.exists(binary)) {
            return true;
        }
    }
    
    return false;
}

QString TarballParser::findDesktopFile(const QString& extractDir) {
    QString dataDir = QString("%1/data").arg(extractDir);
    QString mainDir = findMainDirectory(dataDir);
    
    // Search locations in order of preference
    QStringList searchDirs = {
        QString("%1/usr/share/applications").arg(mainDir),
        QString("%1/share/applications").arg(mainDir),
        mainDir,
        dataDir
    };
    
    for (const QString& searchDir : searchDirs) {
        QDir dir(searchDir);
        if (dir.exists()) {
            QStringList desktopFiles = dir.entryList({"*.desktop"}, QDir::Files);
            if (!desktopFiles.isEmpty()) {
                return dir.absoluteFilePath(desktopFiles.first());
            }
        }
    }
    
    // Recursive search
    QDirIterator it(dataDir, {"*.desktop"}, QDir::Files, QDirIterator::Subdirectories);
    if (it.hasNext()) {
        return it.next();
    }
    
    return QString();
}

QString TarballParser::parseDesktopFile(const QString& desktopPath, PackageMetadata& metadata) {
    QFile file(desktopPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString();
    }
    
    QTextStream in(&file);
    bool inDesktopEntry = false;
    QString execCommand;
    QString iconName;
    
    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        
        if (line.startsWith("[Desktop Entry]")) {
            inDesktopEntry = true;
            continue;
        }
        
        if (line.startsWith("[") && !line.startsWith("[Desktop Entry]")) {
            inDesktopEntry = false;
            continue;
        }
        
        if (!inDesktopEntry) continue;
        
        if (line.startsWith("Exec=")) {
            execCommand = line.mid(5).trimmed();
            execCommand.remove(QRegularExpression("%[fFuUicckdDnNvm]"));
            execCommand = execCommand.trimmed();
            metadata.desktopExecCommand = execCommand;
        } else if (line.startsWith("Icon=")) {
            iconName = line.mid(5).trimmed();
        } else if (line.startsWith("Name=") && metadata.package.isEmpty()) {
            metadata.package = line.mid(5).trimmed();
        } else if (line.startsWith("Comment=") && metadata.description.isEmpty()) {
            metadata.description = line.mid(8).trimmed();
        } else if (line.startsWith("GenericName=") && metadata.description.isEmpty()) {
            metadata.description = line.mid(12).trimmed();
        }
    }
    
    file.close();
    
    // Set icon path if found
    if (!iconName.isEmpty() && metadata.iconPath.isEmpty()) {
        QString dataDir = QFileInfo(desktopPath).absolutePath();
        // Go up to find base directory
        while (!dataDir.isEmpty() && !dataDir.endsWith("/data")) {
            dataDir = QFileInfo(dataDir).absolutePath();
        }
        
        if (!dataDir.isEmpty()) {
            QStringList iconSearchPaths = {
                QString("%1/usr/share/pixmaps/%2").arg(dataDir).arg(iconName),
                QString("%1/usr/share/icons/%2").arg(dataDir).arg(iconName),
                QString("%1/share/pixmaps/%2").arg(dataDir).arg(iconName),
                QString("%1/share/icons/%2").arg(dataDir).arg(iconName)
            };
            
            for (const QString& path : iconSearchPaths) {
                if (QFileInfo::exists(path)) {
                    metadata.iconPath = path;
                    break;
                }
                // Try with extensions
                for (const QString& ext : {"png", "svg", "xpm", "ico"}) {
                    QString pathWithExt = QString("%1.%2").arg(path).arg(ext);
                    if (QFileInfo::exists(pathWithExt)) {
                        metadata.iconPath = pathWithExt;
                        break;
                    }
                }
                if (!metadata.iconPath.isEmpty()) break;
            }
        }
    }
    
    return execCommand;
}

bool TarballParser::isElfExecutable(const QString& filePath) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    
    QByteArray header = file.read(4);
    file.close();
    
    // ELF magic: 0x7F 'E' 'L' 'F'
    return header.size() >= 4 && 
           static_cast<unsigned char>(header[0]) == 0x7F && 
           header[1] == 'E' && header[2] == 'L' && header[3] == 'F';
}

bool TarballParser::isScript(const QString& filePath) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    
    QByteArray header = file.read(2);
    file.close();
    
    return header.size() >= 2 && header[0] == '#' && header[1] == '!';
}

QStringList TarballParser::searchInDirectory(const QString& dir, const QStringList& patterns, bool executableOnly) {
    QStringList results;
    
    QDir dirObj(dir);
    if (!dirObj.exists()) {
        return results;
    }
    
    for (const QString& pattern : patterns) {
        QFileInfoList entries = dirObj.entryInfoList({pattern}, 
            QDir::Files | (executableOnly ? QDir::Executable : QDir::Files));
        
        for (const QFileInfo& entry : entries) {
            if (!executableOnly || entry.isExecutable() || isElfExecutable(entry.absoluteFilePath())) {
                results.append(entry.absoluteFilePath());
            }
        }
        
        // Recursively search subdirectories
        QFileInfoList subdirs = dirObj.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo& subdir : subdirs) {
            results.append(searchInDirectory(subdir.absoluteFilePath(), patterns, executableOnly));
        }
    }
    
    return results;
}

QStringList TarballParser::findExecutables(const QString& extractDir) {
    QStringList executables;
    QString dataDir = QString("%1/data").arg(extractDir);
    QString mainDir = findMainDirectory(dataDir);
    
    // Search directories based on structure
    QStringList searchDirs;
    
    switch (m_structure) {
        case TarballStructure::STANDARD:
            searchDirs = {
                QString("%1/usr/bin").arg(mainDir),
                QString("%1/usr/sbin").arg(mainDir),
                QString("%1/bin").arg(mainDir),
                QString("%1/opt").arg(mainDir),
                QString("%1/usr/lib").arg(mainDir),
                QString("%1/usr/libexec").arg(mainDir)
            };
            break;
        
        case TarballStructure::ELECTRON:
        case TarballStructure::FLAT:
        case TarballStructure::APPDIR:
        default:
            searchDirs = {mainDir, dataDir};
            break;
    }
    
    for (const QString& dir : searchDirs) {
        QStringList found = searchInDirectory(dir, {"*"}, true);
        executables.append(found);
    }
    
    // Filter out non-executables
    QStringList filtered;
    for (const QString& exec : executables) {
        QFileInfo info(exec);
        QString fileName = info.fileName();
        QString filePath = exec;
        
        // Skip known non-executables
        if (filePath.contains("/man/") || filePath.contains("/doc/") ||
            filePath.contains("/share/man/") || filePath.contains("/share/doc/") ||
            filePath.contains("/locale/") || filePath.contains("/locales/") ||
            fileName.endsWith(".so") || fileName.endsWith(".a") ||
            fileName.endsWith(".la") || fileName.endsWith(".pak") ||
            fileName.endsWith(".dat") ||
            fileName.endsWith(".json") || fileName.endsWith(".txt") ||
            fileName.endsWith(".md") || fileName.endsWith(".html") ||
            fileName == "chrome-sandbox" || fileName.contains("crashpad")) {
            continue;
        }
        
        // Must be ELF or script
        if (isElfExecutable(exec) || isScript(exec)) {
            filtered.append(exec);
        }
    }
    
    return filtered;
}

QString TarballParser::findIcon(const QString& extractDir) {
    QString dataDir = QString("%1/data").arg(extractDir);
    QString mainDir = findMainDirectory(dataDir);
    
    QStringList iconDirs = {
        QString("%1/usr/share/pixmaps").arg(mainDir),
        QString("%1/usr/share/icons").arg(mainDir),
        QString("%1/share/pixmaps").arg(mainDir),
        QString("%1/share/icons").arg(mainDir),
        mainDir,
        dataDir
    };
    
    QStringList iconPatterns = {"*.png", "*.svg", "*.xpm", "*.ico"};
    
    for (const QString& iconDir : iconDirs) {
        QStringList icons = searchInDirectory(iconDir, iconPatterns, false);
        
        // Prefer larger icons
        QString bestIcon;
        int bestSize = 0;
        
        for (const QString& icon : icons) {
            QString lowerPath = icon.toLower();
            // Skip small icons
            if (lowerPath.contains("16x16") || lowerPath.contains("22x22") ||
                lowerPath.contains("24x24") || lowerPath.contains("32x32")) {
                continue;
            }
            
            int size = 0;
            if (lowerPath.contains("512x512")) size = 512;
            else if (lowerPath.contains("256x256")) size = 256;
            else if (lowerPath.contains("128x128")) size = 128;
            else if (lowerPath.contains("64x64")) size = 64;
            else if (lowerPath.contains("48x48")) size = 48;
            else size = 100;  // Default for icons without size in path
            
            if (size > bestSize) {
                bestSize = size;
                bestIcon = icon;
            }
        }
        
        if (!bestIcon.isEmpty()) {
            return bestIcon;
        }
        
        // If no best icon found, return first one
        if (!icons.isEmpty()) {
            return icons.first();
        }
    }
    
    return QString();
}

QString TarballParser::guessPackageName(const QString& tarballPath, const QString& extractDir) {
    QFileInfo info(tarballPath);
    QString baseName = info.baseName();
    
    // Remove common suffixes
    baseName.remove(QRegularExpression(R"(\.(tar|linux|x86_64|amd64|x64|linux64)$)", QRegularExpression::CaseInsensitiveOption));
    
    // Try to extract name from filename pattern: name-version or name_version
    QRegularExpression versionRegex(R"(^([a-zA-Z][a-zA-Z0-9_-]*?)[-_](\d+[\d.]*\d*).*$)");
    QRegularExpressionMatch match = versionRegex.match(baseName);
    
    if (match.hasMatch()) {
        return match.captured(1);
    }
    
    // Check if there's only one directory in extraction - use its name
    QString dataDir = QString("%1/data").arg(extractDir);
    QDir dir(dataDir);
    QStringList entries = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    if (entries.size() == 1) {
        QString dirName = entries.first();
        // Clean up directory name
        QRegularExpressionMatch dirMatch = versionRegex.match(dirName);
        if (dirMatch.hasMatch()) {
            return dirMatch.captured(1);
        }
        return dirName;
    }
    
    return baseName;
}

QString TarballParser::guessVersion(const QString& tarballPath, const QString& extractDir) {
    QFileInfo info(tarballPath);
    QString baseName = info.baseName();
    
    // Try to extract version from filename
    QRegularExpression versionRegex(R"([-_](\d+[\d.]*\d*))");
    QRegularExpressionMatch match = versionRegex.match(baseName);
    
    if (match.hasMatch()) {
        return match.captured(1);
    }
    
    // Check directory name in extraction
    QString dataDir = QString("%1/data").arg(extractDir);
    QDir dir(dataDir);
    QStringList entries = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    if (entries.size() == 1) {
        QRegularExpressionMatch dirMatch = versionRegex.match(entries.first());
        if (dirMatch.hasMatch()) {
            return dirMatch.captured(1);
        }
    }
    
    return "1.0";  // Default version
}

PackageMetadata TarballParser::parseMetadata(const QString& extractDir) {
    PackageMetadata metadata;
    QString dataDir = QString("%1/data").arg(extractDir);
    QString mainDir = findMainDirectory(dataDir);
    
    // Try to find and parse .desktop file first
    QString desktopFile = findDesktopFile(extractDir);
    if (!desktopFile.isEmpty()) {
        parseDesktopFile(desktopFile, metadata);
    }
    
    // Find executables
    metadata.executables = findExecutables(extractDir);
    
    // Find icon
    if (metadata.iconPath.isEmpty()) {
        metadata.iconPath = findIcon(extractDir);
    }
    
    // Guess package name if not found
    if (metadata.package.isEmpty()) {
        // Use parent directory name or tarball name
        metadata.package = guessPackageName(m_sourcePath.isEmpty() ? m_tempDir : m_sourcePath, extractDir);
    }
    
    // Guess version if not found
    if (metadata.version.isEmpty()) {
        metadata.version = guessVersion(m_sourcePath.isEmpty() ? m_tempDir : m_sourcePath, extractDir);
    }
    
    // Determine main executable
    if (!metadata.executables.isEmpty()) {
        QString packageLower = metadata.package.toLower();
        
        // Filter helper executables
        QStringList filteredExecutables;
        for (const QString& exec : metadata.executables) {
            QFileInfo execInfo(exec);
            QString fileName = execInfo.fileName().toLower();
            
            if (fileName == "chrome-sandbox" ||
                fileName.contains("crashpad") ||
                fileName.contains("helper") ||
                fileName.contains("nacl") ||
                fileName.endsWith("-bin")) {
                continue;
            }
            
            filteredExecutables.append(exec);
        }
        
        if (filteredExecutables.isEmpty()) {
            filteredExecutables = metadata.executables;
        }

        if (!metadata.desktopExecCommand.isEmpty()) {
            metadata.mainExecutable = resolveExecutableFromCommand(metadata.desktopExecCommand, filteredExecutables);
        }
        
        // Prefer executable with same name as package
        for (const QString& exec : filteredExecutables) {
            if (!metadata.mainExecutable.isEmpty()) {
                break;
            }
            QFileInfo execInfo(exec);
            QString fileName = execInfo.fileName().toLower();
            
            if (fileName == packageLower ||
                fileName.contains(packageLower) ||
                packageLower.contains(fileName)) {
                metadata.mainExecutable = exec;
                break;
            }
        }
        
        // Use first filtered executable if no match
        if (metadata.mainExecutable.isEmpty() && !filteredExecutables.isEmpty()) {
            metadata.mainExecutable = filteredExecutables.first();
        }
    }
    
    return metadata;
}

