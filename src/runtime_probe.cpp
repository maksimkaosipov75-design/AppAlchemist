#include "runtime_probe.h"
#include "appdetector.h"
#include "utils.h"
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

namespace {

struct ProbeCommand {
    QString program;
    QStringList arguments;
    QString workingDirectory;
    int timeoutMs = 4000;
    QStringList successPatterns;
    QStringList failurePatterns;
};

QString shortenOutput(QString text) {
    text = text.trimmed();
    if (text.size() > 400) {
        text = text.left(400) + "...";
    }
    return text;
}

QString resolveAppDirPath(const QString& appDirPath, const QString& candidate) {
    if (candidate.isEmpty()) {
        return QString();
    }

    const QFileInfo info(candidate);
    if (info.isAbsolute()) {
        return candidate;
    }

    if (candidate.startsWith("usr/") || candidate.startsWith("opt/")) {
        return QDir(appDirPath).absoluteFilePath(candidate);
    }

    return QDir(appDirPath).absoluteFilePath(candidate);
}

QString resolveJavaBinary(const QString& appDirPath) {
    const QStringList directCandidates = {
        QDir(appDirPath).absoluteFilePath("usr/bin/java"),
        QDir(appDirPath).absoluteFilePath("usr/lib/jvm/default-java/bin/java"),
        QDir(appDirPath).absoluteFilePath("opt/java/bin/java")
    };

    for (const QString& candidate : directCandidates) {
        const QFileInfo info(candidate);
        if (info.exists() && info.isFile() && info.isExecutable()) {
            return candidate;
        }
    }

    QDirIterator it(appDirPath, {"java"}, QDir::Files | QDir::Executable, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString candidate = it.next();
        if (candidate.contains("/bin/java")) {
            return candidate;
        }
    }

    const QFileInfo systemJava("/usr/bin/java");
    if (systemJava.exists() && systemJava.isExecutable()) {
        return systemJava.absoluteFilePath();
    }

    return QString();
}

QString resolveJavaJarPath(const QString& appDirPath,
                           const PackageMetadata& metadata,
                           const QString& primaryExecutable) {
    QStringList candidates;
    if (primaryExecutable.endsWith(".jar", Qt::CaseInsensitive)) {
        candidates << primaryExecutable;
    }

    if (!metadata.mainExecutable.isEmpty()) {
        candidates << resolveAppDirPath(appDirPath, metadata.mainExecutable);
    }

    for (const QString& exec : metadata.executables) {
        candidates << resolveAppDirPath(appDirPath, exec);
    }

    for (const QString& candidate : candidates) {
        const QFileInfo info(candidate);
        if (info.exists() && info.isFile() && info.suffix().compare("jar", Qt::CaseInsensitive) == 0) {
            return candidate;
        }
    }

    QDirIterator it(appDirPath, {"*.jar"}, QDir::Files, QDirIterator::Subdirectories);
    if (it.hasNext()) {
        return it.next();
    }

    return QString();
}

QString resolvePythonScriptPath(const QString& appDirPath,
                                const QString& primaryExecutable,
                                const PackageMetadata& metadata) {
    if (AppDetector::isPython(primaryExecutable)) {
        return primaryExecutable;
    }

    QString pythonFile;
    if (AppDetector::isPythonLauncherScript(primaryExecutable)) {
        pythonFile = AppDetector::findPythonFileInScript(primaryExecutable);
    }

    QStringList candidates;
    if (!pythonFile.isEmpty()) {
        candidates << pythonFile;
        const QFileInfo pyInfo(pythonFile);
        candidates << QFileInfo(primaryExecutable).dir().absoluteFilePath(pythonFile);
        candidates << QDir(appDirPath).absoluteFilePath(pythonFile);
        candidates << QDir(appDirPath).absoluteFilePath(QString("usr/bin/%1").arg(pyInfo.fileName()));
        candidates << QDir(appDirPath).absoluteFilePath(QString("usr/share/%1").arg(pyInfo.fileName()));
        candidates << QDir(appDirPath).absoluteFilePath(QString("usr/games/%1").arg(pyInfo.fileName()));
    }

    if (!metadata.mainExecutable.isEmpty()) {
        candidates << resolveAppDirPath(appDirPath, metadata.mainExecutable);
    }

    for (const QString& exec : metadata.executables) {
        candidates << resolveAppDirPath(appDirPath, exec);
    }

    for (const QString& candidate : candidates) {
        const QFileInfo info(candidate);
        if (info.exists() && info.isFile() && info.suffix().compare("py", Qt::CaseInsensitive) == 0) {
            return info.absoluteFilePath();
        }
    }

    QDirIterator it(appDirPath, {"*.py"}, QDir::Files, QDirIterator::Subdirectories);
    if (it.hasNext()) {
        return it.next();
    }

    return QString();
}

QString resolvePythonInterpreterPath(const QString& appDirPath) {
    const QString interpreter = AppDetector::findPythonInterpreter(appDirPath);
    if (interpreter == "python3" || interpreter == "python") {
        return interpreter;
    }

    return resolveAppDirPath(appDirPath, interpreter);
}

void appendUnique(QStringList& list, const QString& value) {
    if (!value.isEmpty() && !list.contains(value)) {
        list << value;
    }
}

QProcessEnvironment buildProbeEnvironment(const QString& appDirPath,
                                          const QString& javaBinary,
                                          const QString& pythonInterpreter) {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("APPDIR", appDirPath);
    env.insert("HERE", appDirPath);
    env.insert("NO_AT_BRIDGE", "1");

    QStringList ldParts = {
        QDir(appDirPath).absoluteFilePath("usr/lib"),
        QDir(appDirPath).absoluteFilePath("usr/lib64")
    };
    const QString currentLd = env.value("LD_LIBRARY_PATH");
    if (!currentLd.isEmpty()) {
        ldParts << currentLd;
    }
    env.insert("LD_LIBRARY_PATH", ldParts.join(":"));

    QStringList pathParts = {
        QDir(appDirPath).absoluteFilePath("usr/bin"),
        QDir(appDirPath).absoluteFilePath("usr/sbin"),
        QDir(appDirPath).absoluteFilePath("usr/games")
    };
    const QString currentPath = env.value("PATH");
    if (!currentPath.isEmpty()) {
        pathParts << currentPath;
    }
    env.insert("PATH", pathParts.join(":"));

    if (!javaBinary.isEmpty() && javaBinary.startsWith(appDirPath)) {
        const QString javaHome = QFileInfo(javaBinary).dir().absolutePath();
        env.insert("JAVA_HOME", QFileInfo(javaHome).dir().absolutePath());
    }

    if (!pythonInterpreter.isEmpty() && pythonInterpreter.startsWith(appDirPath)) {
        const QString pythonHome = QFileInfo(pythonInterpreter).dir().absolutePath();
        env.insert("PYTHONHOME", QFileInfo(pythonHome).dir().absolutePath());
    }

    QStringList pythonPaths;
    const QStringList pythonCandidates = {
        "usr/lib/python3",
        "usr/lib/python3/dist-packages",
        "usr/lib/python3/site-packages",
        "usr/share/python3"
    };

    for (const QString& relativePath : pythonCandidates) {
        const QString absolutePath = QDir(appDirPath).absoluteFilePath(relativePath);
        if (QDir(absolutePath).exists()) {
            appendUnique(pythonPaths, absolutePath);
        }
    }

    const QString usrLibPath = QDir(appDirPath).absoluteFilePath("usr/lib");
    if (QDir(usrLibPath).exists()) {
        const QStringList entries = QDir(usrLibPath).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString& entry : entries) {
            if (entry.startsWith("python")) {
                appendUnique(pythonPaths, QDir(appDirPath).absoluteFilePath(QString("usr/lib/%1").arg(entry)));
                appendUnique(pythonPaths, QDir(appDirPath).absoluteFilePath(QString("usr/lib/%1/dist-packages").arg(entry)));
                appendUnique(pythonPaths, QDir(appDirPath).absoluteFilePath(QString("usr/lib/%1/site-packages").arg(entry)));
            }
        }
    }

    if (!pythonPaths.isEmpty()) {
        const QString currentPythonPath = env.value("PYTHONPATH");
        if (!currentPythonPath.isEmpty()) {
            pythonPaths << currentPythonPath;
        }
        env.insert("PYTHONPATH", pythonPaths.join(":"));
    }

    return env;
}

bool probeAppRunSyntax(const QString& appRunPath) {
    QFile file(appRunPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }

    const QByteArray firstLine = file.readLine();
    file.close();

    if (!firstLine.startsWith("#!")) {
        return true;
    }

    if (firstLine.contains("sh") || firstLine.contains("bash")) {
        const ProcessResult syntaxResult = SubprocessWrapper::execute("/bin/sh", {"-n", appRunPath}, {}, 5000);
        return syntaxResult.success;
    }

    return true;
}

void addElectronChecks(RuntimeProbeResult& result,
                       const QString& appDirPath,
                       const QString& primaryExecutable,
                       ApplicationProfile profile) {
    const QFileInfo launcherInfo(primaryExecutable);
    if (!launcherInfo.exists() || !launcherInfo.isFile()) {
        result.fileChecksPassed = false;
        result.failures << "Electron probe failed: primary launcher is missing inside AppDir.";
    } else {
        result.checks << QString("Launcher present: %1").arg(primaryExecutable);
    }

    const QString baseDir = AppDetector::findElectronBaseDir(appDirPath);
    if (baseDir.isEmpty()) {
        result.fileChecksPassed = false;
        result.failures << "Electron probe failed: could not locate Electron base directory.";
        return;
    }

    const QString absoluteBaseDir = QDir(appDirPath).absoluteFilePath(baseDir);
    result.checks << QString("Electron base directory: %1").arg(absoluteBaseDir);

    const bool hasResourcesDir = QDir(QDir(absoluteBaseDir).absoluteFilePath("resources")).exists();
    const bool hasResourcesPak = QFileInfo::exists(QDir(absoluteBaseDir).absoluteFilePath("resources.pak"));
    const bool hasAsar = QFileInfo::exists(QDir(absoluteBaseDir).absoluteFilePath("resources/app.asar"));
    if (!hasResourcesDir && !hasResourcesPak && !hasAsar) {
        result.fileChecksPassed = false;
        result.failures << "Electron probe failed: resources payload is missing.";
    } else {
        result.checks << "Electron resources payload detected.";
    }

    const QString sandboxPath = QDir(absoluteBaseDir).absoluteFilePath("chrome-sandbox");
    if (QFileInfo::exists(sandboxPath)) {
        const QFileInfo sandboxInfo(sandboxPath);
        if (!sandboxInfo.isExecutable()) {
            result.fileChecksPassed = false;
            result.failures << "Electron probe failed: chrome-sandbox exists but is not executable.";
        } else {
            result.checks << QString("Sandbox helper ready: %1").arg(sandboxPath);
        }
    } else if (profile == ApplicationProfile::Chrome) {
        result.warnings << "Chrome-style package does not bundle chrome-sandbox; runtime may rely on --no-sandbox.";
    } else {
        result.warnings << "Electron package does not bundle chrome-sandbox.";
    }
}

void addPythonChecks(RuntimeProbeResult& result,
                     const QString& appDirPath,
                     const QString& primaryExecutable,
                     const PackageMetadata& metadata,
                     QString& pythonInterpreter,
                     QString& pythonScript) {
    pythonInterpreter = resolvePythonInterpreterPath(appDirPath);
    pythonScript = resolvePythonScriptPath(appDirPath, primaryExecutable, metadata);

    if (pythonInterpreter.isEmpty()) {
        result.fileChecksPassed = false;
        result.failures << "Python probe failed: interpreter was not found.";
    } else if (pythonInterpreter == "python3" || pythonInterpreter == "python") {
        result.warnings << QString("Python probe is using system interpreter: %1").arg(pythonInterpreter);
    } else if (!QFileInfo::exists(pythonInterpreter)) {
        result.fileChecksPassed = false;
        result.failures << QString("Python probe failed: bundled interpreter is missing: %1").arg(pythonInterpreter);
    } else {
        result.checks << QString("Python interpreter ready: %1").arg(pythonInterpreter);
    }

    if (pythonScript.isEmpty() || !QFileInfo::exists(pythonScript)) {
        result.fileChecksPassed = false;
        result.failures << "Python probe failed: target script path was not resolved inside AppDir.";
    } else {
        result.checks << QString("Python script ready: %1").arg(pythonScript);
    }
}

void addJavaChecks(RuntimeProbeResult& result,
                   const QString& appDirPath,
                   const PackageMetadata& metadata,
                   const QString& primaryExecutable,
                   QString& javaBinary,
                   QString& jarPath) {
    javaBinary = resolveJavaBinary(appDirPath);
    jarPath = resolveJavaJarPath(appDirPath, metadata, primaryExecutable);

    if (javaBinary.isEmpty()) {
        result.fileChecksPassed = false;
        result.failures << "Java probe failed: no bundled or system java binary is available.";
    } else if (javaBinary.startsWith(appDirPath)) {
        result.checks << QString("Java runtime ready: %1").arg(javaBinary);
    } else {
        result.warnings << QString("Java probe is using system runtime: %1").arg(javaBinary);
    }

    if (jarPath.isEmpty() || !QFileInfo::exists(jarPath)) {
        result.fileChecksPassed = false;
        result.failures << "Java probe failed: application jar was not found inside AppDir.";
    } else {
        result.checks << QString("Java jar ready: %1").arg(jarPath);
    }
}

ProbeCommand buildProbeCommand(const QString& appDirPath,
                               const PackageProfile& profile,
                               const QString& appRunPath,
                               const QString& javaBinary,
                               const QString& jarPath,
                               const QString& pythonInterpreter,
                               const QString& pythonScript) {
    ProbeCommand command;
    command.workingDirectory = appDirPath;
    command.failurePatterns = {
        "error while loading shared libraries",
        "cannot open shared object file",
        "No such file or directory",
        "not found",
        "Traceback",
        "ModuleNotFoundError",
        "ImportError",
        "Exception in thread",
        "Could not find or load main class",
        "Unable to access jarfile",
        "Failed to move to new namespace",
        "setuid sandbox",
        "segmentation fault",
        "core dumped"
    };

    switch (profile.applicationProfile) {
    case ApplicationProfile::Electron:
    case ApplicationProfile::Chrome:
        command.program = appRunPath;
        command.arguments = {"--version"};
        command.timeoutMs = 5000;
        command.successPatterns = {"version"};
        command.failurePatterns << "Missing X server" << "Gtk-WARNING" << "cannot open display";
        break;
    case ApplicationProfile::Java:
        command.program = javaBinary;
        command.arguments = {"-jar", jarPath, "--help"};
        command.workingDirectory = jarPath.isEmpty() ? appDirPath : QFileInfo(jarPath).absolutePath();
        command.timeoutMs = 5000;
        command.successPatterns = {"Usage", "usage", "help", "version"};
        break;
    case ApplicationProfile::Python:
        command.program = pythonInterpreter;
        command.arguments = {pythonScript, "--help"};
        command.workingDirectory = pythonScript.isEmpty() ? appDirPath : QFileInfo(pythonScript).absolutePath();
        command.timeoutMs = 5000;
        command.successPatterns = {"Usage", "usage", "help", "version"};
        break;
    case ApplicationProfile::NativeCli:
    case ApplicationProfile::Script:
        command.program = appRunPath;
        command.arguments = {"--help"};
        command.timeoutMs = 3000;
        command.successPatterns = {"Usage", "usage", "help", "version"};
        break;
    case ApplicationProfile::NativeDesktop:
    case ApplicationProfile::SelfContainedTarball:
    case ApplicationProfile::Unknown:
    case ApplicationProfile::Service:
        // Generic desktop launchers often ignore CLI flags and open the UI instead,
        // which makes the probe intrusive and produces false negatives on timeout.
        command.program.clear();
        break;
    }

    return command;
}

QStringList collectMatchedPatterns(const QString& haystack, const QStringList& patterns) {
    QStringList matchedPatterns;
    for (const QString& pattern : patterns) {
        if (haystack.contains(pattern, Qt::CaseInsensitive)) {
            matchedPatterns << pattern;
        }
    }
    return matchedPatterns;
}

}

QString RuntimeProbeResult::commandSummary() const {
    QStringList parts;
    if (!program.isEmpty()) {
        parts << program;
    }
    parts << arguments;
    return parts.join(' ').trimmed();
}

QString RuntimeProbeResult::summary() const {
    QStringList parts;
    parts << QString("profile=%1").arg(profileName);
    parts << QString("syntax=%1").arg(syntaxCheckPassed ? "ok" : "failed");
    parts << QString("files=%1").arg(fileChecksPassed ? "ok" : "failed");
    parts << QString("command=%1").arg(commandExecuted ? "ran" : "skipped");
    if (commandExecuted) {
        parts << QString("exit=%1").arg(exitCode);
    }
    parts << QString("result=%1").arg(success ? "ok" : "failed");
    return parts.join(" | ");
}

RuntimeProbeResult RuntimeProbePolicy::probe(const QString& appDirPath,
                                             const PackageProfile& profile,
                                             const PackageMetadata& metadata,
                                             const QString& primaryExecutable) {
    RuntimeProbeResult result;
    result.profileName = PackageClassifier::applicationProfileToString(profile.applicationProfile);

    const QString appRunPath = QDir(appDirPath).absoluteFilePath("AppRun");
    if (!QFileInfo::exists(appRunPath)) {
        result.failures << "Runtime probe failed: AppRun is missing.";
        return result;
    }

    result.syntaxCheckPassed = probeAppRunSyntax(appRunPath);
    if (!result.syntaxCheckPassed) {
        result.failures << "Runtime probe failed: AppRun shell syntax check failed.";
        return result;
    }

    QString javaBinary;
    QString jarPath;
    QString pythonInterpreter;
    QString pythonScript;

    switch (profile.applicationProfile) {
    case ApplicationProfile::Electron:
    case ApplicationProfile::Chrome:
        addElectronChecks(result, appDirPath, primaryExecutable, profile.applicationProfile);
        break;
    case ApplicationProfile::Python:
        addPythonChecks(result, appDirPath, primaryExecutable, metadata, pythonInterpreter, pythonScript);
        break;
    case ApplicationProfile::Java:
        addJavaChecks(result, appDirPath, metadata, primaryExecutable, javaBinary, jarPath);
        break;
    default:
        break;
    }

    if (!result.fileChecksPassed) {
        return result;
    }

    const ProbeCommand command = buildProbeCommand(appDirPath,
                                                   profile,
                                                   appRunPath,
                                                   javaBinary,
                                                   jarPath,
                                                   pythonInterpreter,
                                                   pythonScript);
    result.program = command.program;
    result.arguments = command.arguments;
    result.workingDirectory = command.workingDirectory;

    if (command.program.isEmpty()) {
        result.success = true;
        result.warnings << "Runtime probe skipped: no profile-specific command was selected.";
        return result;
    }

    const QProcessEnvironment env = buildProbeEnvironment(appDirPath, javaBinary, pythonInterpreter);
    const ProcessResult processResult = SubprocessWrapper::execute(command.program,
                                                                   command.arguments,
                                                                   command.workingDirectory,
                                                                   command.timeoutMs,
                                                                   env);
    result.commandExecuted = true;
    result.exitCode = processResult.exitCode;
    result.stdoutOutput = shortenOutput(processResult.stdoutOutput);
    result.stderrOutput = shortenOutput(processResult.stderrOutput);

    const QString combinedOutput = processResult.stdoutOutput + "\n" + processResult.stderrOutput;
    const QStringList matchedFailurePatterns = collectMatchedPatterns(combinedOutput, command.failurePatterns);
    result.matchedFailurePattern = !matchedFailurePatterns.isEmpty();
    if (result.matchedFailurePattern) {
        result.failures << QString("Runtime probe matched failure patterns: %1")
                               .arg(matchedFailurePatterns.join(", "));
    }

    const QStringList matchedSuccessPatterns = collectMatchedPatterns(combinedOutput, command.successPatterns);
    result.recognizedHealthyOutput = !matchedSuccessPatterns.isEmpty();
    if (result.recognizedHealthyOutput) {
        result.checks << QString("Recognized healthy probe output: %1").arg(matchedSuccessPatterns.join(", "));
    }

    if (result.matchedFailurePattern) {
        result.success = false;
        return result;
    }

    result.success = processResult.success || result.recognizedHealthyOutput;
    if (!result.success) {
        const QString detail = processResult.errorMessage.isEmpty()
            ? QString("runtime probe returned exit code %1").arg(processResult.exitCode)
            : processResult.errorMessage.trimmed();
        result.failures << QString("Runtime probe command failed: %1").arg(detail);
    }

    return result;
}
