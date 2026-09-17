#ifndef APPDETECTOR_H
#define APPDETECTOR_H

#include "debparser.h"
#include <QString>
#include <QStringList>
#include <QDir>

// Application type detection
enum class AppType {
    Unknown,
    Electron,      // Electron-based applications (VS Code, Codium, etc.)
    Java,          // Java applications (.jar)
    Python,        // Python applications (.py)
    Script,         // Shell scripts
    Native,         // Native executables
    Chrome          // Chrome/Chromium (special case for sandbox)
};

// Application information structure
struct AppInfo {
    AppType type;
    QString baseDir;           // Base directory (e.g., "usr/share/code", "opt/yandex-music")
    QString executablePath;     // Path to main executable
    QString workingDir;          // Working directory for execution
    QStringList envVars;        // Additional environment variables
    bool needsSandbox;           // Whether sandbox setup is needed
    bool needsElectronPath;      // Whether VSCODE_PATH or similar is needed
};

class AppDetector {
public:
    // Detect application type and extract information
    static AppInfo detectApp(const QString& appDirPath, 
                             const QString& extractedDebDir,
                             const QString& mainExecutable,
                             const PackageMetadata& metadata);
    
    // Check if a directory contains an Electron application
    static bool isElectronApp(const QString& dirPath);
    
    // Find Electron base directory (where electron binary and resources are)
    static QString findElectronBaseDir(const QString& appDirPath);
    
    // Check if executable is a script
    static bool isScript(const QString& executablePath);
    
    // Check if executable is Java
    static bool isJava(const QString& executablePath);
    
    // Check if executable is Python
    static bool isPython(const QString& executablePath);
    
    // Check if script is a Python launcher (shell script that runs Python)
    static bool isPythonLauncherScript(const QString& scriptPath);
    
    // Find Python file in launcher script
    static QString findPythonFileInScript(const QString& scriptPath);
    
    // Extract working directory from script
    static QString extractWorkingDirFromScript(const QString& scriptPath);
    
    // Find Python interpreter
    static QString findPythonInterpreter(const QString& appDirPath);
    
    // Rewrites absolute references inside a script so they resolve inside the
    // bundle. The result is relative to the bundle's usr directory, which the
    // generated launcher changes into: unlike a ${HERE} substitution this works
    // in any language, not just in shell scripts. Only paths that actually
    // exist in the AppDir are touched, so references to host tools survive.
    static QString replaceScriptPaths(const QString& scriptContent, 
                                     const QString& appBaseDir,
                                      const QString& appDirPath = QString());
    
    // Find Electron binary location
    static QString findElectronBinary(const QString& baseDir);

    // Architecture normalization and validation (REL-LOW-51)
    static bool isValidArchitecture(const QString& arch);
    static QString normalizeArchitecture(const QString& arch);

    // Search for icon and desktop file across extraction root and data/ subpath (REL-MED-22)
    static QString findIcon(const QString& extractDir);
    static QString findDesktopFile(const QString& extractDir);

private:
    // Check for Electron indicators
    static bool hasElectronIndicators(const QString& dirPath);
};

#endif // APPDETECTOR_H

