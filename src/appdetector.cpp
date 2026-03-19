#include "appdetector.h"
#include "utils.h"
#include <QFileInfo>
#include <QDirIterator>
#include <QRegularExpression>
#include <QDebug>

AppInfo AppDetector::detectApp(const QString& appDirPath,
                               const QString& extractedDebDir,
                               const QString& mainExecutable,
                               const PackageMetadata& metadata) {
    AppInfo info;
    info.type = AppType::Unknown;
    info.needsSandbox = false;
    info.needsElectronPath = false;
    
    QFileInfo execInfo(mainExecutable);
    QString execName = execInfo.fileName();
    QString execPath = mainExecutable;
    
    // Check for Java applications
    if (isJava(mainExecutable)) {
        info.type = AppType::Java;
        info.executablePath = mainExecutable;
        return info;
    }
    
    // Check for Python applications (direct .py files or scripts that launch Python)
    if (isPython(mainExecutable) || isPythonLauncherScript(mainExecutable)) {
        info.type = AppType::Python;
        
        // If it's a launcher script, find the actual Python file
        if (isPythonLauncherScript(mainExecutable)) {
            QString pythonFile = findPythonFileInScript(mainExecutable);
            if (!pythonFile.isEmpty()) {
                info.executablePath = pythonFile;
            } else {
                info.executablePath = mainExecutable;
            }
        } else {
            info.executablePath = mainExecutable;
        }
        
        // Determine working directory for Python apps
        // Check if script specifies a directory
        QString scriptWorkingDir = extractWorkingDirFromScript(mainExecutable);
        if (!scriptWorkingDir.isEmpty()) {
            info.workingDir = scriptWorkingDir;
        } else {
            QString relativeExecPath = info.executablePath;
            if (relativeExecPath.contains("/data/")) {
                relativeExecPath = relativeExecPath.section("/data/", 1);
            }
            if (relativeExecPath.contains("/usr/share/")) {
                // Extract usr/share/appname
                QString path = relativeExecPath;
                if (path.startsWith("/")) {
                    path = path.section("/usr/share/", 1);
                }
                path = path.section('/', 0, 0); // Get first directory
                info.workingDir = QString("${HERE}/usr/share/%1").arg(path);
            } else if (relativeExecPath.contains("/usr/games/")) {
                info.workingDir = "${HERE}/usr/games";
            } else if (relativeExecPath.contains("/usr/bin/")) {
                info.workingDir = "${HERE}/usr/bin";
            } else if (relativeExecPath.contains("/opt/")) {
                QString path = relativeExecPath;
                if (path.startsWith("/")) {
                    path = path.section("/opt/", 1);
                }
                path = path.section('/', 0, 0);
                info.workingDir = QString("${HERE}/opt/%1").arg(path);
            } else {
                info.workingDir = "${HERE}/usr/bin";
            }
        }
        return info;
    }
    
    // Check for Chrome/Chromium (special case) - BEFORE Electron check
    if (execPath.contains("/opt/google/chrome/") || 
        execPath.contains("/opt/chromium/") ||
        execName == "chrome" || execName == "chromium") {
        info.type = AppType::Chrome;
        info.baseDir = "opt/google/chrome";
        info.executablePath = mainExecutable;
        info.workingDir = "${HERE}/opt/google/chrome";
        info.needsSandbox = true;
        return info;
    }
    
    // Check for Electron applications - BEFORE Script check
    // Many Electron apps have binaries in /usr/bin that are NOT shell scripts
    QString electronBaseDir = findElectronBaseDir(appDirPath);
    if (!electronBaseDir.isEmpty()) {
        info.type = AppType::Electron;
        info.baseDir = electronBaseDir;
        info.executablePath = mainExecutable;
        info.needsElectronPath = true;
        
        // Determine working directory
        QString relativeExecPath = mainExecutable;
        if (relativeExecPath.contains("/data/")) {
            relativeExecPath = relativeExecPath.section("/data/", 1);
        }
        if (relativeExecPath.startsWith("/")) {
            if (relativeExecPath.contains("/usr/share/")) {
                // VS Code style: usr/share/code/bin/code
                QString path = relativeExecPath.section("/usr/share/", 1);
                path = path.section('/', 0, -2); // Remove filename
                info.workingDir = QString("${HERE}/usr/share/%1").arg(path);
            } else if (relativeExecPath.contains("/opt/")) {
                // Opt-based: opt/yandex-music
                QString path = relativeExecPath.section("/opt/", 1);
                path = path.section('/', 0, 0); // Get first directory
                info.workingDir = QString("${HERE}/opt/%1").arg(path);
            } else {
                info.workingDir = "${HERE}/usr/bin";
            }
        } else {
            // Already relative
            if (relativeExecPath.contains("usr/share/")) {
                QString path = relativeExecPath.section('/', 0, -2);
                info.workingDir = QString("${HERE}/%1").arg(path);
            } else if (relativeExecPath.contains("opt/")) {
                QString path = relativeExecPath.section('/', 0, 1);
                info.workingDir = QString("${HERE}/%1").arg(path);
            } else {
                info.workingDir = "${HERE}/usr/bin";
            }
        }
        
        return info;
    }
    
    // Check for scripts - AFTER Electron check (Electron apps often have launchers in /bin/)
    if (isScript(mainExecutable)) {
        info.type = AppType::Script;
        info.executablePath = mainExecutable;
        return info;
    }
    
    // Default: Native application
    info.type = AppType::Native;
    info.executablePath = mainExecutable;
    
    // Determine working directory based on path
    QString relativeExecPath = mainExecutable;
    if (relativeExecPath.contains("/data/")) {
        relativeExecPath = relativeExecPath.section("/data/", 1);
    }
    
    // Check for usr/games first (before general /usr/ check)
    if (relativeExecPath.contains("/usr/games/")) {
        // Games should run from usr/games directory
        info.workingDir = "${HERE}/usr/games";
        qDebug() << "Detected game in usr/games, workingDir:" << info.workingDir;
    } else if (relativeExecPath.contains("/opt/")) {
        QString path = relativeExecPath;
        if (path.startsWith("/")) {
            path = path.section("/opt/", 1);
        }
        path = path.section('/', 0, 0); // Get first directory after opt/
        info.workingDir = QString("${HERE}/opt/%1").arg(path);
    } else if (relativeExecPath.contains("/usr/bin/") || relativeExecPath.contains("/usr/sbin/")) {
        info.workingDir = "${HERE}/usr/bin";
    } else if (relativeExecPath.contains("usr/games/")) {
        // Also check for relative path format
        info.workingDir = "${HERE}/usr/games";
        qDebug() << "Detected game in usr/games (relative), workingDir:" << info.workingDir;
    } else {
        // Default to usr/bin
        info.workingDir = "${HERE}/usr/bin";
    }
    
    return info;
}

bool AppDetector::isElectronApp(const QString& dirPath) {
    return hasElectronIndicators(dirPath);
}

QString AppDetector::findElectronBaseDir(const QString& appDirPath) {
    // Check common Electron locations
    QStringList electronDirs = {
        "usr/share/discord",
        "usr/share/code",
        "usr/share/code-oss",
        "usr/share/codium",
        "usr/share/slack",
        "usr/share/teams",
        "usr/share/signal-desktop",
        "usr/share/spotify",
        "usr/share/skypeforlinux",
        "usr/lib/discord",
        "usr/lib/yandex-music",
        "opt/discord",
        "opt/yandex-music",
        "opt/slack",
        "opt/teams",
        "opt/spotify"
    };
    
    // First, check known locations
    for (const QString& dir : electronDirs) {
        QString fullPath = QString("%1/%2").arg(appDirPath).arg(dir);
        if (hasElectronIndicators(fullPath)) {
            return dir;
        }
    }
    
    // Search recursively in usr/share and opt for Electron indicators
    QStringList searchDirs = {
        QString("%1/usr/share").arg(appDirPath),
        QString("%1/opt").arg(appDirPath),
        QString("%1/usr/lib").arg(appDirPath)
    };
    
    for (const QString& searchDir : searchDirs) {
        if (!QDir(searchDir).exists()) continue;
        
        QDirIterator it(searchDir, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            QString path = it.next();
            QFileInfo info(path);
            if (info.isDir() && hasElectronIndicators(path)) {
                // Get relative path from appDirPath
                QString relativePath = QDir(appDirPath).relativeFilePath(path);
                return relativePath;
            }
        }
    }
    
    return QString();
}

bool AppDetector::isScript(const QString& executablePath) {
    // Don't treat Python scripts as shell scripts
    if (isPython(executablePath)) {
        return false;
    }
    
    QFileInfo info(executablePath);
    
    // Check by extension first
    if (info.suffix().toLower() == "sh" || info.fileName().endsWith(".sh")) {
        return true;
    }
    
    // For files in /bin/ or /sbin/, check if they're actually shell scripts
    // by looking at the first bytes (magic bytes or shebang)
    if (info.isFile() && QFile::exists(executablePath)) {
        QFile file(executablePath);
        if (file.open(QIODevice::ReadOnly)) {
            QByteArray header = file.read(4);
            file.close();
            
            // Check for ELF magic bytes (0x7F 'E' 'L' 'F') - NOT a script
            if (header.size() >= 4 && 
                header[0] == 0x7F && 
                header[1] == 'E' && 
                header[2] == 'L' && 
                header[3] == 'F') {
                return false;  // This is an ELF binary, not a script
            }
            
            // Check for shebang (#!) - this IS a script
            if (header.size() >= 2 && header[0] == '#' && header[1] == '!') {
                // Make sure it's not a Python shebang (already handled by isPython)
                file.open(QIODevice::ReadOnly);
                QByteArray firstLine = file.readLine();
                file.close();
                if (!firstLine.contains("python")) {
                    return true;  // Shell script
                }
            }
        }
    }
    
    return false;
}

bool AppDetector::isJava(const QString& executablePath) {
    return executablePath.endsWith(".jar", Qt::CaseInsensitive);
}

bool AppDetector::isPython(const QString& executablePath) {
    // Check by file extension
    if (executablePath.endsWith(".py", Qt::CaseInsensitive)) {
        return true;
    }
    
    // Check by shebang
    QFile file(executablePath);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QByteArray firstLine = file.readLine();
        file.close();
        if (firstLine.startsWith("#!") && 
            (firstLine.contains("/python") || firstLine.contains("python"))) {
            return true;
        }
    }
    
    return false;
}

bool AppDetector::isPythonLauncherScript(const QString& scriptPath) {
    QFile file(scriptPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }
    
    QString content = file.readAll();
    file.close();
    
    // Check if script contains Python execution patterns
    QRegularExpression pythonPatterns[] = {
        QRegularExpression(R"(python3?\s+[\w/\.-]+\.py)"),
        QRegularExpression(R"(exec\s+python3?)"),
        QRegularExpression(R"(python3?\s+"?\$[^"]*\.py)"),
        QRegularExpression(R"(cd\s+.*\s*;\s*python3?\s+.*\.py)"),
        QRegularExpression(R"(python3?\s+.*\.py\s*"\$@")")
    };
    
    for (const auto& pattern : pythonPatterns) {
        if (pattern.match(content).hasMatch()) {
            return true;
        }
    }
    
    return false;
}

QString AppDetector::findPythonFileInScript(const QString& scriptPath) {
    QFile file(scriptPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString();
    }
    
    QString content = file.readAll();
    file.close();
    
    // First, try to extract directory from cd command
    QString cdDir;
    QRegularExpression cdPattern(R"(cd\s+([/\w-]+))");
    QRegularExpressionMatch cdMatch = cdPattern.match(content);
    if (cdMatch.hasMatch()) {
        cdDir = cdMatch.captured(1);
    }
    
    // Try to find .py file in script - look for patterns like:
    // python3 file.py
    // python3 /path/to/file.py
    // python file.py "$@"
    QList<QRegularExpression> pyFilePatterns;
    pyFilePatterns << QRegularExpression(R"(python3?\s+([/\w\.-]+\.py))");  // python3 file.py or python3 /path/file.py
    pyFilePatterns << QRegularExpression(QStringLiteral("python3?\\s+\"([/\\w\\.-]+\\.py)\"")); // python3 "/path/file.py"
    pyFilePatterns << QRegularExpression(QStringLiteral("python3?\\s+'([/\\w\\.-]+\\.py)'")); // python3 '/path/file.py'
    
    for (const auto& pattern : pyFilePatterns) {
        QRegularExpressionMatch match = pattern.match(content);
        if (match.hasMatch()) {
            QString pyFile = match.captured(1);
            // If it's a relative path and we have cd directory, combine them
            if (!pyFile.startsWith("/") && !cdDir.isEmpty()) {
                return QString("%1/%2").arg(cdDir).arg(pyFile);
            } else if (pyFile.startsWith("/")) {
                return pyFile;
            } else {
                // Relative path without cd - use as is
                return pyFile;
            }
        }
    }
    
    // Fallback: try simple pattern
    QRegularExpression simplePattern(R"(([\w/\.-]+\.py))");
    QRegularExpressionMatch match = simplePattern.match(content);
    if (match.hasMatch()) {
        QString pyFile = match.captured(1);
        if (!pyFile.startsWith("/") && !cdDir.isEmpty()) {
            return QString("%1/%2").arg(cdDir).arg(pyFile);
        }
        return pyFile;
    }
    
    return QString();
}

QString AppDetector::extractWorkingDirFromScript(const QString& scriptPath) {
    QFile file(scriptPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString();
    }
    
    QString content = file.readAll();
    file.close();
    
    // Look for cd command
    QRegularExpression cdPattern(R"(cd\s+([/\w-]+))");
    QRegularExpressionMatch match = cdPattern.match(content);
    if (match.hasMatch()) {
        QString cdDir = match.captured(1);
        // Convert absolute path to ${HERE} relative
        if (cdDir.startsWith("/usr/share/")) {
            QString relPath = cdDir.section("/usr/share/", 1);
            return QString("${HERE}/usr/share/%1").arg(relPath);
        } else if (cdDir.startsWith("/usr/games/")) {
            return "${HERE}/usr/games";
        } else if (cdDir.startsWith("/usr/bin/")) {
            return "${HERE}/usr/bin";
        } else if (cdDir.startsWith("/opt/")) {
            QString relPath = cdDir.section("/opt/", 1);
            return QString("${HERE}/opt/%1").arg(relPath);
        }
    }
    
    return QString();
}

QString AppDetector::findPythonInterpreter(const QString& appDirPath) {
    // First, try to find Python in AppDir
    QStringList pythonPaths = {
        QString("%1/usr/bin/python3").arg(appDirPath),
        QString("%1/usr/bin/python").arg(appDirPath),
        QString("%1/opt/python3/bin/python3").arg(appDirPath),
        QString("%1/opt/python/bin/python").arg(appDirPath)
    };
    
    for (const QString& path : pythonPaths) {
        if (QFile::exists(path)) {
            // Get relative path
            QString relativePath = QDir(appDirPath).relativeFilePath(path);
            return relativePath;
        }
    }
    
    // If not found, use system Python
    return "python3";
}

QString AppDetector::replaceScriptPaths(const QString& scriptContent,
                                        const QString& appBaseDir) {
    QString result = scriptContent;
    
    // Replace common absolute paths with ${HERE} relative paths
    // This is a universal replacement that works for most scripts
    
    // Replace /usr/lib/ paths (but preserve ${HERE}/usr/lib/)
    result.replace(QRegularExpression(R"((?<!\$\{HERE\})/usr/lib/)"), "${HERE}/usr/lib/");
    
    // Replace /usr/bin/ paths
    result.replace(QRegularExpression(R"((?<!\$\{HERE\})/usr/bin/)"), "${HERE}/usr/bin/");
    
    // Replace /opt/ paths
    result.replace(QRegularExpression(R"((?<!\$\{HERE\})/opt/)"), "${HERE}/opt/");
    
    // Replace /usr/share/ paths
    result.replace(QRegularExpression(R"((?<!\$\{HERE\})/usr/share/)"), "${HERE}/usr/share/");
    
    // Replace specific app base directory paths
    if (!appBaseDir.isEmpty()) {
        QString absBasePath = QString("/%1/").arg(appBaseDir);
        QString hereBasePath = QString("${HERE}/%1/").arg(appBaseDir);
        result.replace(absBasePath, hereBasePath);
    }
    
    // Replace common variable assignments
    result.replace(QRegularExpression(R"((ELECTRON_BIN|APP_DIR|INSTALL_DIR)=([^:]*):-?/usr/lib/)"), 
                  R"(\1=\2:-${HERE}/usr/lib/)");
    
    // Replace cp commands with absolute paths
    result.replace(QRegularExpression(R"(cp\s+/usr/lib/)"), "cp \"${HERE}/usr/lib/");
    result.replace(QRegularExpression(R"(cp\s+/opt/)"), "cp \"${HERE}/opt/");
    
    return result;
}

bool AppDetector::hasElectronIndicators(const QString& dirPath) {
    if (!QDir(dirPath).exists()) return false;
    
    // Check for Electron indicators
    QStringList electronFiles = {
        "snapshot_blob.bin",
        "v8_context_snapshot.bin",
        "resources.pak",
        "electron",
        "Discord",          // Discord executable
        "discord",          // discord lowercase
        "code",             // VS Code executable
        "codium",           // VSCodium executable
        "slack",            // Slack
        "teams",            // Teams
        "spotify",          // Spotify
        "chrome_crashpad_handler"  // Common in Electron/Chrome apps
    };
    
    // Check for electron binary or resources
    for (const QString& file : electronFiles) {
        QString filePath = QString("%1/%2").arg(dirPath).arg(file);
        if (QFile::exists(filePath)) {
            return true;
        }
    }
    
    // Check for resources directory
    if (QDir(QString("%1/resources").arg(dirPath)).exists()) {
        return true;
    }
    
    // Check for .asar files (Electron app bundles)
    QDirIterator it(dirPath, {"*.asar"}, QDir::Files, QDirIterator::Subdirectories);
    if (it.hasNext()) {
        return true;
    }
    
    return false;
}

QString AppDetector::findElectronBinary(const QString& fullBaseDirPath) {
    // Safety check: if path is empty or contains dangerous patterns
    if (fullBaseDirPath.isEmpty() || fullBaseDirPath.contains("..")) {
        return QString();
    }
    
    // Check if directory exists
    QDir baseDir(fullBaseDirPath);
    if (!baseDir.exists()) {
        return QString();
    }
    
    // Common Electron binary names - include app-specific names
    QStringList possibleNames = {
        "electron", 
        "Discord", "discord",  // Discord
        "code", "codium",      // VS Code
        "Slack", "slack",      // Slack
        "teams", "Teams",      // Teams
        "spotify", "Spotify",  // Spotify
        "signal-desktop"       // Signal
    };
    
    // First check in electron/ subdirectory (common for packaged Electron apps)
    QString electronSubdir = QString("%1/electron").arg(fullBaseDirPath);
    if (QDir(electronSubdir).exists()) {
        for (const QString& name : possibleNames) {
            QString path = QString("%1/%2").arg(electronSubdir).arg(name);
            QFileInfo info(path);
            if (info.exists() && info.isExecutable()) {
                return QString("electron/%1").arg(name);
            }
        }
    }
    
    // Then check directly in base directory
    for (const QString& name : possibleNames) {
        QString path = QString("%1/%2").arg(fullBaseDirPath).arg(name);
        QFileInfo info(path);
        if (info.exists() && info.isExecutable()) {
            return name;
        }
    }
    
    // Check in bin/ subdirectory
    QString binSubdir = QString("%1/bin").arg(fullBaseDirPath);
    if (QDir(binSubdir).exists()) {
        for (const QString& name : possibleNames) {
            QString path = QString("%1/%2").arg(binSubdir).arg(name);
            QFileInfo info(path);
            if (info.exists() && info.isExecutable()) {
                return QString("bin/%1").arg(name);
            }
        }
    }
    
    return QString();
}
