#include "rpmparser.h"
#include "utils.h"
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFile>
#include <QTextStream>
#include <QStandardPaths>
#include <QDebug>
#include <QRegularExpression>
#include <QDateTime>
#include <QIODevice>

RpmParser::RpmParser() {
    QString tempBase = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    m_tempDir = QString("%1/appalchemist-rpm-%2").arg(tempBase).arg(QString::number(QDateTime::currentMSecsSinceEpoch()));
}

RpmParser::~RpmParser() {
    if (!m_tempDir.isEmpty() && QDir(m_tempDir).exists()) {
        SubprocessWrapper::removeDirectory(m_tempDir);
    }
}

bool RpmParser::validateRpmFile(const QString& rpmPath) {
    QFileInfo info(rpmPath);
    if (!info.exists() || !info.isFile()) {
        return false;
    }
    
    // Check extension
    if (!info.suffix().toLower().endsWith("rpm")) {
        return false;
    }
    
    // Check magic number (RPM packages start with "ed ab ee db" or "drpm")
    QFile file(rpmPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    
    QByteArray header = file.read(4);
    file.close();
    
    // RPM magic numbers: 0xed 0xab 0xee 0xdb (old) or "drpm" (new)
    if (header.size() >= 4 && 
        ((header[0] == (char)0xed && header[1] == (char)0xab && 
          header[2] == (char)0xee && header[3] == (char)0xdb) ||
         header.startsWith("drpm"))) {
        return true;
    }
    
    return false;
}

bool RpmParser::extractRpm(const QString& rpmPath, const QString& extractDir) {
    if (!validateRpmFile(rpmPath)) {
        return false;
    }
    
    // Create extraction directory
    if (!SubprocessWrapper::createDirectory(extractDir)) {
        return false;
    }
    
    // Extract RPM using rpm2cpio and cpio
    // First, try to use rpm2cpio if available
    QString cpioDir = QString("%1/cpio").arg(extractDir);
    if (!SubprocessWrapper::createDirectory(cpioDir)) {
        return false;
    }
    
    // Check if rpm2cpio is available
    ProcessResult checkRpm2cpio = SubprocessWrapper::execute("which", {"rpm2cpio"});
    bool hasRpm2cpio = checkRpm2cpio.success && !checkRpm2cpio.stdoutOutput.trimmed().isEmpty();
    
    // Check if cpio is available
    ProcessResult checkCpio = SubprocessWrapper::execute("which", {"cpio"});
    bool hasCpio = checkCpio.success && !checkCpio.stdoutOutput.trimmed().isEmpty();
    
    QString extractPath = QString("%1/data").arg(extractDir);
    if (!SubprocessWrapper::createDirectory(extractPath)) {
        return false;
    }
    
    if (hasRpm2cpio && hasCpio) {
        // Use rpm2cpio | cpio
        // Pipe rpm2cpio output to cpio, cd into extract directory first
        ProcessResult cpioResult = SubprocessWrapper::execute("sh", {
            "-c", 
            QString("cd '%1' && rpm2cpio '%2' | cpio -idmv 2>&1").arg(extractPath).arg(rpmPath)
        }, extractPath);
        
        // Check if extraction was successful by looking for extracted files
        QDir extractPathDir(extractPath);
        bool hasFiles = extractPathDir.exists() && 
                       (!extractPathDir.entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
        
        // Also check if we got a .cpio file (wrong extraction)
        QStringList files = extractPathDir.entryList(QDir::Files, QDir::Name);
        bool hasCpioFile = false;
        for (const QString& file : files) {
            if (file.endsWith(".cpio", Qt::CaseInsensitive)) {
                hasCpioFile = true;
                qWarning() << "WARNING: Found .cpio file instead of extracted contents:" << file;
                break;
            }
        }
        
        if (hasCpioFile || (!cpioResult.success && !hasFiles)) {
            qWarning() << "cpio extraction failed or produced .cpio file";
            qWarning() << "cpio stdout:" << cpioResult.stdoutOutput;
            qWarning() << "cpio stderr:" << cpioResult.stderrOutput;
            // Try alternative: use bsdtar if available (some RPMs can be extracted with it)
            ProcessResult bsdtarCheck = SubprocessWrapper::execute("which", {"bsdtar"});
            if (bsdtarCheck.success && !bsdtarCheck.stdoutOutput.trimmed().isEmpty()) {
                qWarning() << "Trying bsdtar as fallback...";
                // Clean up failed extraction
                SubprocessWrapper::removeDirectory(extractPath);
                SubprocessWrapper::createDirectory(extractPath);
                ProcessResult bsdtarResult = SubprocessWrapper::execute("bsdtar", {
                    "-xf", rpmPath, "-C", extractPath
                });
                if (bsdtarResult.success) {
                    QDir newExtractPathDir(extractPath);
                    if (!newExtractPathDir.entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).isEmpty()) {
                        qWarning() << "bsdtar extraction successful";
                        return true;
                    }
                }
            }
            return false;
        }
        
        // Log extraction result for debugging
        if (extractPathDir.exists()) {
            QStringList entries = extractPathDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
            qDebug() << "Extracted RPM structure - top level dirs:" << entries;
        }
    } else {
        // Fallback: try using bsdtar (available on Arch Linux and can extract RPMs)
        ProcessResult bsdtarCheck = SubprocessWrapper::execute("which", {"bsdtar"});
        if (bsdtarCheck.success && !bsdtarCheck.stdoutOutput.trimmed().isEmpty()) {
            qWarning() << "rpm2cpio not found, trying bsdtar...";
            ProcessResult bsdtarResult = SubprocessWrapper::execute("bsdtar", {
                "-xf", rpmPath, "-C", extractPath
            });
            if (bsdtarResult.success) {
                QDir extractPathDir(extractPath);
                QStringList entries = extractPathDir.entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
                if (!entries.isEmpty()) {
                    qWarning() << "bsdtar extraction successful, found" << entries.size() << "entries";
                    return true;
                }
            }
            qWarning() << "bsdtar extraction failed:" << bsdtarResult.errorMessage;
        }
        
        // Try using rpm command directly (if available)
        ProcessResult rpmResult = SubprocessWrapper::execute("rpm", {
            "--query", "--package", "--list", rpmPath
        });
        
        if (!rpmResult.success) {
            // Last resort: try using 7z or unzip (some RPMs can be extracted this way)
            ProcessResult sevenZipResult = SubprocessWrapper::execute("7z", {
                "x", rpmPath, "-o" + extractPath
            });
            
            if (!sevenZipResult.success) {
                qWarning() << "ERROR: Cannot extract RPM. Please install rpmextract package (contains rpm2cpio)";
                qWarning() << "  On Arch Linux: sudo pacman -S rpmextract";
                return false;
            }
        } else {
            // Use rpm to extract
            ProcessResult extractResult = SubprocessWrapper::execute("rpm", {
                "--install", "--root", extractPath, "--nodeps", "--notriggers", "--noscripts", rpmPath
            }, extractPath);
            
            if (!extractResult.success) {
                qWarning() << "RPM extraction failed:" << extractResult.errorMessage;
                return false;
            }
        }
    }
    
    return true;
}

PackageMetadata RpmParser::parseMetadata(const QString& extractDir) {
    PackageMetadata metadata;
    
    // Try to get metadata from the original RPM file if we have the path
    // This is more reliable than parsing extracted files
    QString rpmPath = m_tempDir; // We need to store the RPM path
    // For now, try to query metadata from extracted structure
    
    // Find .desktop file first (for Java apps and proper entry point)
    QString desktopFile = findDesktopFile(extractDir);
    if (!desktopFile.isEmpty()) {
        parseDesktopFile(desktopFile, metadata);
    }
    
    // Find executables - check both data/ subdirectory and root
    QString dataDir = QString("%1/data").arg(extractDir);
    QString rootDir = extractDir;
    
    qDebug() << "RpmParser::parseMetadata - extractDir:" << extractDir;
    qDebug() << "RpmParser::parseMetadata - dataDir exists:" << QDir(dataDir).exists();
    qDebug() << "RpmParser::parseMetadata - rootDir exists:" << QDir(rootDir).exists();
    
    // Try data directory first (like DEB structure)
    if (QDir(dataDir).exists()) {
        qDebug() << "Searching executables in dataDir:" << dataDir;
        metadata.executables = findExecutables(dataDir);
        qDebug() << "Found" << metadata.executables.size() << "executables in dataDir";
    }
    
    // If no executables in data/, try root directory (RPM structure)
    if (metadata.executables.isEmpty()) {
        qDebug() << "No executables in dataDir, searching in rootDir:" << rootDir;
        metadata.executables = findExecutables(rootDir);
        qDebug() << "Found" << metadata.executables.size() << "executables in rootDir";
    }
    
    // Find scripts
    metadata.scripts = findScripts(extractDir);
    
    // Find icon - check both locations
    if (metadata.iconPath.isEmpty()) {
        if (QDir(dataDir).exists()) {
            metadata.iconPath = findIcon(dataDir);
        }
        if (metadata.iconPath.isEmpty()) {
            metadata.iconPath = findIcon(rootDir);
        }
    }
    
    // Determine main executable
    if (!metadata.executables.isEmpty()) {
        // Filter out utility/helper executables
        QStringList filteredExecutables;
        for (const QString& exec : metadata.executables) {
            QFileInfo execInfo(exec);
            QString fileName = execInfo.fileName();
            QString filePath = exec;
            
            // Skip utility executables and non-executable files
            if (fileName == "chrome-sandbox" ||
                fileName == "codium-tunnel" ||
                fileName.contains("crashpad") ||
                fileName.contains("crash-handler") ||
                fileName.endsWith(".desktop") ||
                fileName.endsWith(".xml") ||
                fileName.endsWith(".node") ||
                fileName.contains("policy-watcher") ||
                fileName.contains("watcher") ||
                fileName.contains("helper") ||
                (fileName.contains("launcher") && !fileName.contains(metadata.package.toLower()))) {
                continue;
            }
            
            filteredExecutables.append(exec);
        }
        
        // If we filtered everything, use original list
        if (filteredExecutables.isEmpty()) {
            filteredExecutables = metadata.executables;
        }

        if (!metadata.desktopExecCommand.isEmpty()) {
            const QString resolvedExec = resolveExecutableFromCommand(metadata.desktopExecCommand, filteredExecutables);
            if (!resolvedExec.isEmpty()) {
                metadata.mainExecutable = resolvedExec;
            }
        }
        
        // Prefer executable with same name as package
        for (const QString& exec : filteredExecutables) {
            if (!metadata.mainExecutable.isEmpty()) {
                break;
            }
            QFileInfo execInfo(exec);
            QString baseName = execInfo.baseName().toLower();
            QString packageName = metadata.package.toLower();
            
            if (baseName == packageName || 
                baseName.contains(packageName) ||
                packageName.contains(baseName)) {
                metadata.mainExecutable = exec;
                break;
            }
        }
        
        // If still not found, prefer files in usr/share/*/bin (wrapper scripts)
        if (metadata.mainExecutable.isEmpty()) {
            for (const QString& exec : filteredExecutables) {
                if (exec.contains("/usr/share/") && exec.contains("/bin/")) {
                    metadata.mainExecutable = exec;
                    break;
                }
            }
        }
        
        // Then prefer files in usr/bin
        if (metadata.mainExecutable.isEmpty()) {
            for (const QString& exec : filteredExecutables) {
                if (exec.contains("/usr/bin/")) {
                    // But skip .desktop files that might have executable flag
                    QFileInfo execInfo(exec);
                    if (!execInfo.fileName().endsWith(".desktop")) {
                        metadata.mainExecutable = exec;
                        break;
                    }
                }
            }
        }
        
        // Last resort: use first filtered executable (but not .desktop)
        if (metadata.mainExecutable.isEmpty() && !filteredExecutables.isEmpty()) {
            for (const QString& exec : filteredExecutables) {
                QFileInfo execInfo(exec);
                if (!execInfo.fileName().endsWith(".desktop")) {
                    metadata.mainExecutable = exec;
                    break;
                }
            }
        }
    }
    
    return metadata;
}

bool RpmParser::parseSpecFile(const QString& specPath, PackageMetadata& metadata) {
    QFile file(specPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }
    
    QTextStream in(&file);
    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        
        if (line.startsWith("Name:")) {
            metadata.package = line.mid(5).trimmed();
        } else if (line.startsWith("Version:")) {
            metadata.version = line.mid(8).trimmed();
        } else if (line.startsWith("Summary:") || line.startsWith("Description:")) {
            if (metadata.description.isEmpty()) {
                metadata.description = line.mid(line.indexOf(':') + 1).trimmed();
            }
        } else if (line.startsWith("Requires:") || line.startsWith("Requires(")) {
            QString deps = line.mid(line.indexOf(':') + 1).trimmed();
            QStringList depList = deps.split(',', Qt::SkipEmptyParts);
            for (QString& dep : depList) {
                dep = dep.trimmed();
                // Remove version constraints
                int spacePos = dep.indexOf(' ');
                if (spacePos > 0) {
                    dep = dep.left(spacePos);
                }
                if (!dep.isEmpty()) {
                    metadata.depends.append(dep);
                }
            }
        }
    }
    
    file.close();
    return true;
}

QStringList RpmParser::findExecutables(const QString& extractDir) {
    QStringList executables;
    
    QStringList searchDirs = {
        QString("%1/usr/bin").arg(extractDir),
        QString("%1/usr/sbin").arg(extractDir),
        QString("%1/opt").arg(extractDir),
        QString("%1/usr/local/bin").arg(extractDir),
        QString("%1/bin").arg(extractDir),
        QString("%1/sbin").arg(extractDir),
        QString("%1/usr/games").arg(extractDir),
        QString("%1/usr/lib").arg(extractDir),
        QString("%1/usr/libexec").arg(extractDir),
        QString("%1/usr/share").arg(extractDir)  // Some apps put executables here
    };
    
    // First, try to find files with executable flag (but exclude .desktop files)
    for (const QString& dir : searchDirs) {
        if (QDir(dir).exists()) {
            QStringList found = searchInDirectory(dir, {"*"}, true);
            // Filter out .desktop files immediately
            for (const QString& file : found) {
                QFileInfo fileInfo(file);
                if (!fileInfo.fileName().endsWith(".desktop")) {
                    executables.append(file);
                }
            }
        }
    }
    
    // If no executables found with executable flag, try to find ELF binaries or scripts
    if (executables.isEmpty()) {
        for (const QString& dir : searchDirs) {
            if (QDir(dir).exists()) {
                QStringList allFiles = searchInDirectory(dir, {"*"}, false);
                for (const QString& file : allFiles) {
                    if (isElfExecutable(file) || isScriptFile(file)) {
                        executables.append(file);
                    }
                }
            }
        }
    }
    
    // If still no executables, do a recursive search in the entire directory
    if (executables.isEmpty()) {
        qDebug() << "No executables found in standard locations, doing recursive search in:" << extractDir;
        QDirIterator it(extractDir, QDirIterator::Subdirectories | QDirIterator::FollowSymlinks);
        while (it.hasNext()) {
            QString filePath = it.next();
            QFileInfo fileInfo(filePath);
            
            if (fileInfo.isFile()) {
                // Skip files in certain directories
                if (filePath.contains("/man/") || 
                    filePath.contains("/doc/") || 
                    filePath.contains("/share/man/") ||
                    filePath.contains("/share/doc/") ||
                    filePath.contains("/locale/") ||
                    filePath.contains("/i18n/")) {
                    continue;
                }
                
                // Check if it's an executable or potential executable
                if (fileInfo.isExecutable() || 
                    isElfExecutable(filePath) || 
                    isScriptFile(filePath) ||
                    fileInfo.suffix() == "jar") {
                    // Check if it's in a bin-like directory
                    if (filePath.contains("/bin/") || 
                        filePath.contains("/sbin/") || 
                        filePath.contains("/libexec/") ||
                        filePath.contains("/opt/") ||
                        fileInfo.suffix() == "jar") {
                        executables.append(filePath);
                        qDebug() << "Found executable via recursive search:" << filePath;
                    }
                }
            }
        }
    }
    
    // Also search for .jar files (Java applications)
    QStringList jarDirs = {
        QString("%1/usr/games").arg(extractDir),
        QString("%1/opt").arg(extractDir),
        QString("%1/usr/lib").arg(extractDir),
        QString("%1/usr/share").arg(extractDir)
    };
    
    for (const QString& dir : jarDirs) {
        QStringList jarFiles = searchInDirectory(dir, {"*.jar"}, false);
        for (const QString& jar : jarFiles) {
            if (!jar.contains("/lib/") && !jar.contains("/jre/") && !jar.contains("/man/")) {
                executables.append(jar);
            }
        }
    }
    
        // Filter out non-executable files
        QStringList filtered;
        for (const QString& exec : executables) {
            QFileInfo execInfo(exec);
            QString fileName = execInfo.fileName();
            QString filePath = exec;
            
            // First check: skip known non-executable file types
            if (filePath.contains("/man/") || 
                filePath.contains("/doc/") || 
                filePath.contains("/share/man/") ||
                filePath.contains("/share/doc/") ||
                filePath.contains("/share/applications/") ||
                filePath.contains("/share/appdata/") ||
                filePath.contains("/share/mime/") ||
                fileName.endsWith(".1") || 
                fileName.endsWith(".2") || 
                fileName.endsWith(".3") ||
                fileName.endsWith(".html") ||
                fileName.endsWith(".txt") ||
                fileName.endsWith(".pdf") ||
                fileName.endsWith(".desktop") ||
                fileName.endsWith(".appdata.xml") ||
                fileName.endsWith(".xml") ||
                fileName.endsWith(".pak") ||
                (fileName.endsWith(".bin") && !execInfo.isExecutable() &&
                 !isElfExecutable(exec) && !isScriptFile(exec) &&
                 !fileName.contains("snapshot") && !fileName.contains("v8"))) {
                continue;
            }
            
            if (filePath.contains("/jre/") && 
                !filePath.contains("/jre/bin/") && 
                !filePath.endsWith(".jar")) {
                continue;
            }
            
            // Second check: must be actually executable (ELF, script, or jar)
            bool isExecutable = execInfo.isExecutable() && 
                               (isElfExecutable(exec) || isScriptFile(exec) || fileName.endsWith(".jar"));
            
            // Also check if it's a script by content (shebang)
            if (!isExecutable && isScriptFile(exec)) {
                isExecutable = true;
            }
            
            if (isExecutable) {
                filtered.append(exec);
            }
        }
        
        return filtered;
}

QStringList RpmParser::findScripts(const QString& extractDir) {
    QStringList scripts;
    
    // RPM scripts are usually in %pre, %post, %preun, %postun sections
    // They might be extracted to /var/lib/rpm-state or similar
    // For now, we'll search common locations
    QStringList scriptDirs = {
        QString("%1/var/lib/rpm-state").arg(extractDir),
        QString("%1/etc").arg(extractDir)
    };
    
    for (const QString& dir : scriptDirs) {
        QStringList found = searchInDirectory(dir, {"*pre*", "*post*", "*un*"}, false);
        scripts.append(found);
    }
    
    return scripts;
}

QString RpmParser::findIcon(const QString& extractDir) {
    QStringList iconDirs = {
        QString("%1/usr/share/pixmaps").arg(extractDir),
        QString("%1/usr/share/icons").arg(extractDir),
        QString("%1/usr/share/icons/hicolor").arg(extractDir),
        QString("%1/usr/share/applications").arg(extractDir)
    };
    
    QStringList iconPatterns = {"*.png", "*.svg", "*.xpm", "*.ico"};
    
    for (const QString& dir : iconDirs) {
        for (const QString& pattern : iconPatterns) {
            QStringList icons = searchInDirectory(dir, {pattern}, false);
            if (!icons.isEmpty()) {
                return icons.first();
            }
        }
    }
    
    return QString();
}

QString RpmParser::findDesktopFile(const QString& extractDir) {
    QString dataDir = QString("%1/data").arg(extractDir);
    QString desktopDir = QString("%1/usr/share/applications").arg(dataDir);
    
    QDir dir(desktopDir);
    if (dir.exists()) {
        QStringList desktopFiles = dir.entryList({"*.desktop"}, QDir::Files);
        if (!desktopFiles.isEmpty()) {
            return dir.absoluteFilePath(desktopFiles.first());
        }
    }
    
    // Also check without data/ prefix (RPM structure might differ)
    desktopDir = QString("%1/usr/share/applications").arg(extractDir);
    dir.setPath(desktopDir);
    if (dir.exists()) {
        QStringList desktopFiles = dir.entryList({"*.desktop"}, QDir::Files);
        if (!desktopFiles.isEmpty()) {
            return dir.absoluteFilePath(desktopFiles.first());
        }
    }
    
    return QString();
}

QString RpmParser::parseDesktopFile(const QString& desktopPath, PackageMetadata& metadata) {
    // Same implementation as DebParser
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
        
        if (!inDesktopEntry) {
            continue;
        }
        
        if (line.startsWith("Exec=")) {
            execCommand = line.mid(5).trimmed();
            execCommand.remove(QRegularExpression("%[fFuUicckdDnNvm]"));
            execCommand = execCommand.trimmed();
            metadata.desktopExecCommand = execCommand;
        } else if (line.startsWith("Icon=")) {
            iconName = line.mid(5).trimmed();
        } else if (line.startsWith("Name=") && metadata.description.isEmpty()) {
            metadata.description = line.mid(5).trimmed();
        }
    }
    
    file.close();
    
    // Parse exec command similar to DebParser
    if (!execCommand.isEmpty()) {
        QString execPath = execCommand.split(' ').first();
        if (execPath.startsWith("/")) {
            QString dataDir = QString("%1/data").arg(QFileInfo(desktopPath).absolutePath().section('/', 0, -3));
            QString fullPath = QString("%1%2").arg(dataDir).arg(execPath);
            if (QFileInfo::exists(fullPath)) {
                metadata.mainExecutable = fullPath;
                metadata.executables.append(fullPath);
            } else {
                QFileInfo fullPathInfo(fullPath);
                if (fullPathInfo.isSymLink()) {
                    QString linkTarget = fullPathInfo.symLinkTarget();
                    if (linkTarget.startsWith("/")) {
                        linkTarget = QString("%1%2").arg(dataDir).arg(linkTarget);
                    }
                    if (QFileInfo::exists(linkTarget)) {
                        metadata.mainExecutable = linkTarget;
                        metadata.executables.append(linkTarget);
                    }
                }
            }
        }
    }
    
    return QString();
}

QStringList RpmParser::searchInDirectory(const QString& dir, const QStringList& patterns, bool executableOnly) {
    QStringList results;
    QDir dirObj(dir);
    
    if (!dirObj.exists()) {
        return results;
    }
    
    for (const QString& pattern : patterns) {
        QFileInfoList entries = dirObj.entryInfoList({pattern}, QDir::Files | QDir::Executable, QDir::Name);
        
        for (const QFileInfo& info : entries) {
            if (executableOnly && !info.isExecutable()) {
                continue;
            }
            results.append(info.absoluteFilePath());
        }
    }
    
    // Recursively search subdirectories
    QFileInfoList subdirs = dirObj.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo& subdir : subdirs) {
        results.append(searchInDirectory(subdir.absoluteFilePath(), patterns, executableOnly));
    }
    
    return results;
}

bool RpmParser::isElfExecutable(const QString& filePath) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    
    QByteArray header = file.read(4);
    file.close();
    
    if (header.size() >= 4 && 
        header[0] == 0x7F && 
        header[1] == 'E' && 
        header[2] == 'L' && 
        header[3] == 'F') {
        return true;
    }
    
    if (header.size() >= 2 && header[0] == '#' && header[1] == '!') {
        return true;
    }
    
    return false;
}

bool RpmParser::isScriptFile(const QString& filePath) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    
    QByteArray header = file.read(2);
    file.close();
    
    return header.size() >= 2 && header[0] == '#' && header[1] == '!';
}

QStringList RpmParser::findJavaApplications(const QString& extractDir) {
    QStringList jarFiles;
    
    QStringList searchDirs = {
        QString("%1/usr/share").arg(extractDir),
        QString("%1/opt").arg(extractDir),
        QString("%1/usr/lib").arg(extractDir)
    };
    
    for (const QString& dir : searchDirs) {
        QStringList jars = searchInDirectory(dir, {"*.jar"}, false);
        for (const QString& jar : jars) {
            if (!jar.contains("/lib/") && !jar.contains("/jre/")) {
                jarFiles.append(jar);
            }
        }
    }
    
    return jarFiles;
}
