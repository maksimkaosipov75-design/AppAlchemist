#include "debparser.h"
#include "utils.h"
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QTextStream>
#include <QStandardPaths>
#include <QDebug>
#include <QRegularExpression>
#include <QDateTime>
#include <QIODevice>

DebParser::DebParser() {
    QString tempBase = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    m_tempDir = QString("%1/deb-to-appimage-%2").arg(tempBase).arg(QString::number(QDateTime::currentMSecsSinceEpoch()));
}

DebParser::~DebParser() {
    if (!m_tempDir.isEmpty() && QDir(m_tempDir).exists()) {
        SubprocessWrapper::removeDirectory(m_tempDir);
    }
}

bool DebParser::validateDebFile(const QString& debPath) {
    QFileInfo info(debPath);
    if (!info.exists() || !info.isFile()) {
        return false;
    }
    
    // Check extension
    if (!info.suffix().toLower().endsWith("deb")) {
        return false;
    }
    
    // Check magic number (Debian binary package starts with "!<arch>")
    QFile file(debPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    
    QByteArray header = file.read(8);
    file.close();
    
    // Debian packages are ar archives
    if (header.startsWith("!<arch>")) {
        return true;
    }
    
    return false;
}

bool DebParser::extractDeb(const QString& debPath, const QString& extractDir) {
    if (!validateDebFile(debPath)) {
        return false;
    }
    
    // Create extraction directory
    if (!SubprocessWrapper::createDirectory(extractDir)) {
        return false;
    }
    
    // Extract .deb using ar
    QString arDir = QString("%1/ar").arg(extractDir);
    if (!SubprocessWrapper::createDirectory(arDir)) {
        return false;
    }
    
    ProcessResult arResult = SubprocessWrapper::execute("ar", 
        {"x", debPath}, arDir);
    
    if (!arResult.success) {
        return false;
    }
    
    // Find data.tar.* file
    QDir arDirObj(arDir);
    QStringList tarFiles = arDirObj.entryList({"data.tar.*"}, QDir::Files);
    if (tarFiles.isEmpty()) {
        return false;
    }
    
    QString dataTar = arDirObj.absoluteFilePath(tarFiles.first());
    
    // Extract data.tar.*
    QString extractPath = QString("%1/data").arg(extractDir);
    if (!SubprocessWrapper::createDirectory(extractPath)) {
        return false;
    }
    
    QString tarCommand = "tar";
    QStringList tarArgs;
    
    if (dataTar.endsWith(".gz")) {
        tarArgs = {"-xzf", dataTar, "-C", extractPath};
    } else if (dataTar.endsWith(".xz")) {
        tarArgs = {"-xJf", dataTar, "-C", extractPath};
    } else if (dataTar.endsWith(".bz2")) {
        tarArgs = {"-xjf", dataTar, "-C", extractPath};
    } else if (dataTar.endsWith(".zst") || dataTar.endsWith(".zstd")) {
        tarArgs = {"--zstd", "-xf", dataTar, "-C", extractPath};
    } else {
        tarArgs = {"-xf", dataTar, "-C", extractPath};
    }
    
    ProcessResult tarResult = SubprocessWrapper::execute(tarCommand, tarArgs);
    if (!tarResult.success) {
        return false;
    }
    
    // Extract control.tar.*
    QStringList controlTarFiles = arDirObj.entryList({"control.tar.*"}, QDir::Files);
    if (!controlTarFiles.isEmpty()) {
        QString controlTar = arDirObj.absoluteFilePath(controlTarFiles.first());
        QString controlPath = QString("%1/control").arg(extractDir);
        if (!SubprocessWrapper::createDirectory(controlPath)) {
            return false;
        }
        
        QStringList controlTarArgs;
        if (controlTar.endsWith(".gz")) {
            controlTarArgs = {"-xzf", controlTar, "-C", controlPath};
        } else if (controlTar.endsWith(".xz")) {
            controlTarArgs = {"-xJf", controlTar, "-C", controlPath};
        } else if (controlTar.endsWith(".bz2")) {
            controlTarArgs = {"-xjf", controlTar, "-C", controlPath};
        } else if (controlTar.endsWith(".zst") || controlTar.endsWith(".zstd")) {
            controlTarArgs = {"--zstd", "-xf", controlTar, "-C", controlPath};
        } else {
            controlTarArgs = {"-xf", controlTar, "-C", controlPath};
        }
        
        SubprocessWrapper::execute(tarCommand, controlTarArgs);
    }
    
    return true;
}

bool DebParser::parseControlFile(const QString& controlPath, DebMetadata& metadata) {
    QFile file(controlPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }
    
    QTextStream in(&file);
    QString currentField;
    QString currentValue;
    
    while (!in.atEnd()) {
        QString line = in.readLine();
        
        if (line.isEmpty()) {
            continue;
        }
        
        if (line.startsWith(" ")) {
            // Continuation line
            currentValue += " " + line.trimmed();
        } else {
            // New field
            if (!currentField.isEmpty()) {
                if (currentField == "Package") {
                    metadata.package = currentValue;
                } else if (currentField == "Version") {
                    metadata.version = currentValue;
                } else if (currentField == "Description") {
                    metadata.description = currentValue;
                } else if (currentField == "Depends") {
                    // Parse dependencies (format: pkg1, pkg2 (>= 1.0), pkg3)
                    QStringList deps = currentValue.split(',');
                    for (QString& dep : deps) {
                        dep = dep.trimmed();
                        // Remove version constraints
                        int parenPos = dep.indexOf('(');
                        if (parenPos > 0) {
                            dep = dep.left(parenPos).trimmed();
                        }
                        if (!dep.isEmpty()) {
                            metadata.depends.append(dep);
                        }
                    }
                }
            }
            
            int colonPos = line.indexOf(':');
            if (colonPos > 0) {
                currentField = line.left(colonPos).trimmed();
                currentValue = line.mid(colonPos + 1).trimmed();
            }
        }
    }
    
    // Handle last field
    if (!currentField.isEmpty()) {
        if (currentField == "Package") {
            metadata.package = currentValue;
        } else if (currentField == "Version") {
            metadata.version = currentValue;
        } else if (currentField == "Description") {
            metadata.description = currentValue;
        } else if (currentField == "Depends") {
            QStringList deps = currentValue.split(',');
            for (QString& dep : deps) {
                dep = dep.trimmed();
                int parenPos = dep.indexOf('(');
                if (parenPos > 0) {
                    dep = dep.left(parenPos).trimmed();
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

DebMetadata DebParser::parseMetadata(const QString& extractDir) {
    DebMetadata metadata;
    
    // Find and parse control file
    QString controlDir = QString("%1/control").arg(extractDir);
    QDir controlDirObj(controlDir);
    
    QString controlFile = controlDirObj.absoluteFilePath("control");
    if (QFileInfo::exists(controlFile)) {
        parseControlFile(controlFile, metadata);
    }
    
    // Find .desktop file first (for Java apps and proper entry point)
    QString desktopFile = findDesktopFile(extractDir);
    if (!desktopFile.isEmpty()) {
        parseDesktopFile(desktopFile, metadata);
    }
    
    // Find executables
    QString dataDir = QString("%1/data").arg(extractDir);
    metadata.executables = findExecutables(dataDir);
    
    // Find scripts
    metadata.scripts = findScripts(extractDir);
    
    // Find icon
    if (metadata.iconPath.isEmpty()) {
        metadata.iconPath = findIcon(dataDir);
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

bool DebParser::isElfExecutable(const QString& filePath) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    
    QByteArray header = file.read(4);
    file.close();
    
    // ELF magic number: 0x7F 'E' 'L' 'F'
    if (header.size() >= 4 && 
        header[0] == 0x7F && 
        header[1] == 'E' && 
        header[2] == 'L' && 
        header[3] == 'F') {
        return true;
    }
    
    // Check for shebang (script files)
    if (header.size() >= 2 && header[0] == '#' && header[1] == '!') {
        return true;
    }
    
    return false;
}

QStringList DebParser::findExecutables(const QString& extractDir) {
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
        QString("%1/usr/share").arg(extractDir)
    };
    
    for (const QString& dir : searchDirs) {
        QStringList found = searchInDirectory(dir, {"*"}, true);
        executables.append(found);
    }
    
    // If no executables found with executable flag, try to find ELF binaries
    if (executables.isEmpty()) {
        for (const QString& dir : searchDirs) {
            QStringList allFiles = searchInDirectory(dir, {"*"}, false);
            for (const QString& file : allFiles) {
                if (isElfExecutable(file)) {
                    executables.append(file);
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
            // Check if it's a main JAR (not in lib subdirectory, not in jre)
            if (!jar.contains("/lib/") && !jar.contains("/jre/") && !jar.contains("/man/")) {
                executables.append(jar);
            }
        }
    }
    
    // Filter out non-executable files that might have been incorrectly identified
    QStringList filtered;
    for (const QString& exec : executables) {
        QFileInfo execInfo(exec);
        QString fileName = execInfo.fileName();
        QString filePath = exec;
        
        // Skip man pages, documentation, and other non-executable files
        if (filePath.contains("/man/") || 
            filePath.contains("/doc/") || 
            filePath.contains("/share/man/") ||
            filePath.contains("/share/doc/") ||
            fileName.endsWith(".1") || 
            fileName.endsWith(".2") || 
            fileName.endsWith(".3") ||
            fileName.endsWith(".html") ||
            fileName.endsWith(".txt") ||
            fileName.endsWith(".pdf")) {
            continue;
        }
        
        // Skip files in JRE that are not actual executables
        if (filePath.contains("/jre/") && 
            !filePath.contains("/jre/bin/") && 
            !filePath.endsWith(".jar")) {
            continue;
        }
        
        // Only include actual executables or jar files
        if (execInfo.isExecutable() || 
            isElfExecutable(exec) || 
            fileName.endsWith(".jar") ||
            fileName.startsWith("#!")) {
            filtered.append(exec);
        }
    }
    
    return filtered;
}

QString DebParser::findDesktopFile(const QString& extractDir) {
    QString dataDir = QString("%1/data").arg(extractDir);
    QString desktopDir = QString("%1/usr/share/applications").arg(dataDir);
    
    QDir dir(desktopDir);
    if (dir.exists()) {
        QStringList desktopFiles = dir.entryList({"*.desktop"}, QDir::Files);
        if (!desktopFiles.isEmpty()) {
            return dir.absoluteFilePath(desktopFiles.first());
        }
    }
    
    return QString();
}

QString DebParser::parseDesktopFile(const QString& desktopPath, DebMetadata& metadata) {
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
            // Remove %f, %F, %u, %U, %i, %c, %k, %d, %D, %n, %N, %v, %m
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

    QString dataDir;
    int dataPos = desktopPath.indexOf("/data/");
    if (dataPos >= 0) {
        dataDir = desktopPath.left(dataPos + 5);
    }
    if (dataDir.isEmpty()) {
        dataDir = QFileInfo(desktopPath).absolutePath();
    }
    
    // If Exec contains java or jar, it's a Java application
    if (execCommand.contains("java") || execCommand.contains(".jar")) {
        // Extract jar file path
        QRegularExpression jarRegex(R"(([^\s]+\.jar))");
        QRegularExpressionMatch match = jarRegex.match(execCommand);
        if (match.hasMatch()) {
            QString jarPath = match.captured(1);
            // Resolve relative paths
            if (jarPath.startsWith("/")) {
                QString fullJarPath = QString("%1%2").arg(dataDir).arg(jarPath);
                if (QFileInfo::exists(fullJarPath)) {
                    metadata.mainExecutable = fullJarPath;
                    metadata.executables.append(fullJarPath);
                }
            } else {
                // Try to find jar in common locations
                QStringList searchPaths = {
                    QString("%1/usr/games/%2").arg(dataDir).arg(jarPath),
                    QString("%1/opt/%2").arg(dataDir).arg(jarPath),
                    QString("%1/usr/lib/%2").arg(dataDir).arg(jarPath),
                    QString("%1/usr/share/%2").arg(dataDir).arg(jarPath)
                };
                for (const QString& path : searchPaths) {
                    if (QFileInfo::exists(path)) {
                        metadata.mainExecutable = path;
                        metadata.executables.append(path);
                        break;
                    }
                }
            }
        }
    } else if (!execCommand.isEmpty()) {
        // Regular executable
        QString execPath = execCommand.split(' ').first();
        if (execPath.startsWith("/")) {
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
        } else {
            // Search in PATH locations
            QStringList searchPaths = {
                QString("%1/usr/bin/%2").arg(dataDir).arg(execPath),
                QString("%1/usr/sbin/%2").arg(dataDir).arg(execPath),
                QString("%1/bin/%2").arg(dataDir).arg(execPath),
                QString("%1/usr/share/%2").arg(dataDir).arg(execPath)
            };
            for (const QString& path : searchPaths) {
                if (QFileInfo::exists(path)) {
                    metadata.mainExecutable = path;
                    metadata.executables.append(path);
                    break;
                }
            }
        }
    }
    
    // Find icon if specified
    if (!iconName.isEmpty() && metadata.iconPath.isEmpty()) {
        QStringList iconPaths = {
            QString("%1/usr/share/pixmaps/%2").arg(dataDir).arg(iconName),
            QString("%1/usr/share/icons/%2").arg(dataDir).arg(iconName),
            QString("%1/usr/share/icons/hicolor/256x256/apps/%2").arg(dataDir).arg(iconName)
        };
        for (const QString& path : iconPaths) {
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
    
    return execCommand;
}

QStringList DebParser::findScripts(const QString& extractDir) {
    QStringList scripts;
    
    QString controlDir = QString("%1/control").arg(extractDir);
    QStringList scriptNames = {"postinst", "prerm", "postrm", "preinst"};
    
    for (const QString& scriptName : scriptNames) {
        QString scriptPath = QString("%1/%2").arg(controlDir).arg(scriptName);
        if (QFileInfo::exists(scriptPath)) {
            scripts.append(scriptPath);
        }
    }
    
    return scripts;
}

QString DebParser::findIcon(const QString& extractDir) {
    QStringList iconDirs = {
        QString("%1/usr/share/pixmaps").arg(extractDir),
        QString("%1/usr/share/icons").arg(extractDir),
        QString("%1/usr/share/applications").arg(extractDir)
    };
    
    QStringList iconExtensions = {"*.png", "*.svg", "*.xpm", "*.ico"};
    
    for (const QString& iconDir : iconDirs) {
        QStringList icons = searchInDirectory(iconDir, iconExtensions, false);
        if (!icons.isEmpty()) {
            return icons.first();
        }
    }
    
    return QString();
}

QStringList DebParser::searchInDirectory(const QString& dir, const QStringList& patterns, bool executableOnly) {
    QStringList results;
    
    QDir dirObj(dir);
    if (!dirObj.exists()) {
        return results;
    }
    
    for (const QString& pattern : patterns) {
        QFileInfoList entries = dirObj.entryInfoList(
            {pattern},
            QDir::Files | (executableOnly ? QDir::Executable : QDir::Files),
            QDir::Name
        );
        
        for (const QFileInfo& entry : entries) {
            if (!executableOnly || entry.isExecutable()) {
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
