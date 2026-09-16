#include "utils.h"
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QCryptographicHash>
#include <QStandardPaths>
#include <QThread>
#include <QDebug>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>
#include <unistd.h>
#include <limits.h>

namespace {
QString stripQuotes(QString value) {
    if (value.size() >= 2) {
        const QChar first = value.front();
        const QChar last = value.back();
        if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
            value = value.mid(1, value.size() - 2);
        }
    }
    return value;
}

QString readRawSymlinkTarget(const QString& path) {
    char buf[PATH_MAX];
    ssize_t len = ::readlink(path.toUtf8().constData(), buf, sizeof(buf) - 1);
    if (len != -1) {
        buf[len] = '\0';
        return QString::fromUtf8(buf);
    }
    return QString();
}

QString computeRelativeSymlinkTarget(const QString& symlinkPath, const QString& absoluteTarget,
                                      const QString& extractedRoot, const QString& destRoot) {
    // Convert an absolute symlink target to a relative path that works inside AppDir.
    //
    // Case 1: target is inside the extracted package tree (e.g. /tmp/appalchemist-XXX/data/usr/share/...)
    //   → strip the extracted root and make relative to symlink location in destRoot
    // Case 2: target is an absolute system path (e.g. /usr/share/codium/bin/codium)
    //   → try to find the corresponding file in destRoot and make relative

    QString normalizedTarget = absoluteTarget;
    QString relativeSuffix;

    // Try stripping extractedRoot/data/ prefix first, then extractedRoot/ prefix
    if (!extractedRoot.isEmpty()) {
        QString dataPrefix = extractedRoot + "/data/";
        QString directPrefix = extractedRoot + "/";
        if (normalizedTarget.startsWith(dataPrefix)) {
            relativeSuffix = normalizedTarget.mid(dataPrefix.length());
        } else if (normalizedTarget.startsWith(directPrefix)) {
            relativeSuffix = normalizedTarget.mid(directPrefix.length());
            // Still strip data/ if present
            if (relativeSuffix.startsWith("data/")) relativeSuffix = relativeSuffix.mid(5);
        }
    }

    // If not matched, try common absolute system paths
    if (relativeSuffix.isEmpty()) {
        if (normalizedTarget.startsWith("/usr/") || normalizedTarget.startsWith("/opt/") ||
            normalizedTarget.startsWith("/bin/") || normalizedTarget.startsWith("/lib/") ||
            normalizedTarget.startsWith("/sbin/")) {
            relativeSuffix = normalizedTarget.mid(1);
        }
    }

    if (relativeSuffix.isEmpty()) {
        return QString();
    }

    // Now compute relative path from symlink location to target location within destRoot
    QString destTargetPath = QString("%1/%2").arg(destRoot).arg(relativeSuffix);
    QString symlinkDir = QFileInfo(symlinkPath).absolutePath();

    QDir symlinkDirObj(symlinkDir);
    QString relPath = symlinkDirObj.relativeFilePath(destTargetPath);
    return relPath;
}

bool recreateSymlink(const QFileInfo& sourceInfo, const QString& destination,
                     const QString& extractedRoot = QString(), const QString& destRoot = QString()) {
    // First try to read the raw symlink target (preserving relative paths)
    QString rawTarget = readRawSymlinkTarget(sourceInfo.absoluteFilePath());

    if (!rawTarget.isEmpty() && !rawTarget.startsWith("/")) {
        // Relative symlink: verify it stays strictly confined inside destRoot if destRoot is provided
        if (!destRoot.isEmpty()) {
            const QString symlinkDir = QFileInfo(destination).absolutePath();
            const QString resolved = QDir::cleanPath(symlinkDir + "/" + rawTarget);
            const QString cleanDestRoot = QDir::cleanPath(destRoot);
            if (!resolved.startsWith(cleanDestRoot + "/") && resolved != cleanDestRoot) {
                qWarning() << "Refusing to create escaping relative symlink:" << rawTarget
                           << "resolved to:" << resolved;
                return false;
            }
        }
        QFile::remove(destination);
        return QFile::link(rawTarget, destination);
    }

    // Absolute symlink — need to convert to relative path for portability
    QString absoluteTarget = rawTarget.isEmpty() ? sourceInfo.symLinkTarget() : rawTarget;
    if (absoluteTarget.isEmpty()) {
        return false;
    }

    // Try to compute a relative path within the destination tree
    if (!destRoot.isEmpty()) {
        QString relTarget = computeRelativeSymlinkTarget(destination, absoluteTarget, extractedRoot, destRoot);
        if (!relTarget.isEmpty()) {
            QFile::remove(destination);
            return QFile::link(relTarget, destination);
        }
    }

    // Fallback: if target is a system path that maps into the AppDir, make it relative
    QString fallbackTarget = absoluteTarget;
    if (fallbackTarget.startsWith("/usr/") || fallbackTarget.startsWith("/opt/")) {
        // Strip leading slash to get AppDir-relative path
        QString appDirRelative = fallbackTarget.mid(1); // e.g. "usr/share/codium/codium"
        QString symlinkDir = QFileInfo(destination).absolutePath();
        QDir symlinkDirObj(symlinkDir);
        // We need destRoot to compute the full path
        if (!destRoot.isEmpty()) {
            QString fullTargetInDest = QString("%1/%2").arg(destRoot).arg(appDirRelative);
            QString relPath = symlinkDirObj.relativeFilePath(fullTargetInDest);
            QFile::remove(destination);
            return QFile::link(relPath, destination);
        }
    }

    // Reject dangerous host system targets
    const QString cleanAbsolute = QDir::cleanPath(absoluteTarget);
    if (cleanAbsolute.startsWith("/etc") || cleanAbsolute.startsWith("/root") ||
        cleanAbsolute.startsWith("/home") || cleanAbsolute.startsWith("/var") ||
        cleanAbsolute.startsWith("/tmp") || cleanAbsolute.startsWith("/dev") ||
        cleanAbsolute.startsWith("/proc") || cleanAbsolute.startsWith("/sys")) {
        qWarning() << "Refusing to create symlink to dangerous host path:" << absoluteTarget
                   << "for" << destination;
        return false;
    }

    // Last resort: use absolute target (old behavior for other paths)
    qWarning() << "Could not make symlink relative, using absolute target:" << absoluteTarget
               << "for" << destination;
    QFile::remove(destination);
    return QFile::link(absoluteTarget, destination);
}
}

ProcessResult SubprocessWrapper::execute(const QString& command, 
                                         const QStringList& arguments,
                                         const QString& workingDirectory,
                                         int timeoutMs,
                                         const QProcessEnvironment& environment) {
    ProcessResult result;
    result.success = false;
    
    QProcess process;
    process.setProgram(command);
    process.setArguments(arguments);
    
    if (!workingDirectory.isEmpty()) {
        process.setWorkingDirectory(workingDirectory);
    }
    
    // Set environment variables if provided
    if (!environment.isEmpty()) {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        QStringList envKeys = environment.keys();
        for (const QString& key : envKeys) {
            env.insert(key, environment.value(key));
        }
        process.setProcessEnvironment(env);
    }
    
    process.start();
    
    if (!process.waitForStarted(timeoutMs)) {
        result.errorMessage = QString("Failed to start process: %1").arg(command);
        return result;
    }
    
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(1000);
        result.errorMessage = QString("Process timed out: %1").arg(command);
        return result;
    }
    
    result.exitCode = process.exitCode();
    result.stdoutOutput = QString::fromUtf8(process.readAllStandardOutput());
    result.stderrOutput = QString::fromUtf8(process.readAllStandardError());
    result.success = (process.exitStatus() == QProcess::NormalExit && result.exitCode == 0);
    
    if (!result.success) {
        result.errorMessage = QString("Process failed with exit code %1: %2")
            .arg(result.exitCode)
            .arg(result.stderrOutput.isEmpty() ? result.stdoutOutput : result.stderrOutput);
    }
    
    return result;
}

ProcessResult SubprocessWrapper::executePipeline(const QString& producerCommand,
                                                 const QStringList& producerArguments,
                                                 const QString& consumerCommand,
                                                 const QStringList& consumerArguments,
                                                 const QString& workingDirectory,
                                                 int timeoutMs) {
    ProcessResult result;
    result.success = false;

    QProcess producer;
    QProcess consumer;

    producer.setProgram(producerCommand);
    producer.setArguments(producerArguments);
    producer.setStandardOutputProcess(&consumer);

    consumer.setProgram(consumerCommand);
    consumer.setArguments(consumerArguments);
    if (!workingDirectory.isEmpty()) {
        consumer.setWorkingDirectory(workingDirectory);
    }

    producer.start();
    consumer.start();

    if (!producer.waitForStarted(timeoutMs) || !consumer.waitForStarted(timeoutMs)) {
        producer.kill();
        consumer.kill();
        producer.waitForFinished(1000);
        consumer.waitForFinished(1000);
        result.errorMessage = QString("Failed to start pipeline: %1 | %2")
            .arg(producerCommand, consumerCommand);
        return result;
    }

    if (!producer.waitForFinished(timeoutMs) || !consumer.waitForFinished(timeoutMs)) {
        producer.kill();
        consumer.kill();
        producer.waitForFinished(1000);
        consumer.waitForFinished(1000);
        result.errorMessage = QString("Pipeline timed out: %1 | %2")
            .arg(producerCommand, consumerCommand);
        return result;
    }

    result.exitCode = consumer.exitCode();
    result.stdoutOutput = QString::fromUtf8(consumer.readAllStandardOutput());
    result.stderrOutput = QString::fromUtf8(consumer.readAllStandardError());

    const QString producerErrors = QString::fromUtf8(producer.readAllStandardError());
    if (!producerErrors.isEmpty()) {
        result.stderrOutput += producerErrors;
    }

    result.success = (producer.exitStatus() == QProcess::NormalExit && producer.exitCode() == 0 &&
                      consumer.exitStatus() == QProcess::NormalExit && consumer.exitCode() == 0);
    if (!result.success) {
        result.errorMessage = QString("Pipeline failed (%1 exit %2, %3 exit %4): %5")
            .arg(producerCommand)
            .arg(producer.exitCode())
            .arg(consumerCommand)
            .arg(consumer.exitCode())
            .arg(result.stderrOutput.isEmpty() ? result.stdoutOutput : result.stderrOutput);
    }

    return result;
}

bool SubprocessWrapper::copyFile(const QString& source, const QString& destination) {
    return copyFile(source, destination, QString(), QString());
}

bool SubprocessWrapper::copyFile(const QString& source, const QString& destination,
                                 const QString& extractedRoot, const QString& destRoot) {
    QFileInfo sourceInfo(source);
    if (!sourceInfo.exists() && !sourceInfo.isSymLink()) {
        return false;
    }

    QFileInfo destInfo(destination);
    QDir destDir = destInfo.dir();
    if (!destDir.exists()) {
        destDir.mkpath(".");
    }

    if (QFileInfo::exists(destination) || QFileInfo(destination).isSymLink()) {
        QFile::remove(destination);
    }

    if (sourceInfo.isSymLink()) {
        return recreateSymlink(sourceInfo, destination, extractedRoot, destRoot);
    }

    if (!QFile::copy(source, destination)) {
        return false;
    }

    QFile(destination).setPermissions(sourceInfo.permissions());
    return true;
}

namespace {
bool copyDirectoryHelper(const QString& source, const QString& destination,
                         const QString& extractedRoot, const QString& destRoot,
                         QSet<QString>& visitedDirs) {
    QDir sourceDir(source);
    if (!sourceDir.exists()) {
        qWarning() << "Source directory does not exist:" << source;
        return false;
    }

    QString canonicalSource = QFileInfo(source).canonicalFilePath();
    if (canonicalSource.isEmpty()) {
        canonicalSource = QDir::cleanPath(source);
    }
    if (visitedDirs.contains(canonicalSource)) {
        qWarning() << "Symlink loop / cycle detected in copyDirectory, skipping already visited directory:" << source;
        return true;
    }
    visitedDirs.insert(canonicalSource);

    QDir destDir(destination);
    if (!destDir.exists()) {
        if (!destDir.mkpath(".")) {
            qWarning() << "Failed to create destination directory:" << destination;
            return false;
        }
    }

    const QFileInfoList entries = sourceDir.entryInfoList(
        QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot
    );
    for (const QFileInfo& entry : entries) {
        const QString srcPath = entry.absoluteFilePath();
        const QString destPath = destDir.absoluteFilePath(entry.fileName());

        if (entry.isSymLink()) {
            if (!SubprocessWrapper::copyFile(srcPath, destPath, extractedRoot, destRoot)) {
                qWarning() << "Failed to copy symlink:" << srcPath << "to" << destPath;
            }
            continue;
        }

        if (entry.isDir()) {
            QString canonicalEntry = entry.canonicalFilePath();
            if (canonicalEntry.isEmpty()) {
                canonicalEntry = QDir::cleanPath(srcPath);
            }
            if (visitedDirs.contains(canonicalEntry)) {
                qWarning() << "Symlink loop / cycle detected in copyDirectory, skipping subdirectory:" << srcPath;
                continue;
            }
            if (!copyDirectoryHelper(srcPath, destPath, extractedRoot, destRoot, visitedDirs)) {
                qWarning() << "Failed to copy subdirectory:" << srcPath;
            }
            continue;
        }

        if (!SubprocessWrapper::copyFile(srcPath, destPath, extractedRoot, destRoot)) {
            qWarning() << "Failed to copy file:" << srcPath << "to" << destPath;
        }
    }

    return true;
}
}

bool SubprocessWrapper::copyDirectory(const QString& source, const QString& destination) {
    return copyDirectory(source, destination, QString(), QString());
}

bool SubprocessWrapper::copyDirectory(const QString& source, const QString& destination,
                                      const QString& extractedRoot, const QString& destRoot) {
    QSet<QString> visitedDirs;
    return copyDirectoryHelper(source, destination, extractedRoot, destRoot, visitedDirs);
}

bool SubprocessWrapper::createDirectory(const QString& path) {
    QDir dir;
    bool ok = dir.mkpath(path);
    if (ok) {
        if (path.startsWith(QDir::tempPath()) || path.startsWith("/tmp")) {
            QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        }
    }
    return ok;
}

QString SubprocessWrapper::createTemporaryDirectory(const QString& prefix) {
    const QString effectivePrefix = prefix.isEmpty() ? QStringLiteral("appalchemist") : prefix;
    QTemporaryDir tempDir(QDir::tempPath() + "/" + effectivePrefix + "-XXXXXX");
    tempDir.setAutoRemove(false);
    if (!tempDir.isValid()) {
        return QString();
    }
    const QString path = tempDir.path();
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return path;
}

bool SubprocessWrapper::removeDirectory(const QString& path) {
    QDir dir(path);
    if (!dir.exists()) {
        return true;
    }
    return dir.removeRecursively();
}

QString SubprocessWrapper::generateHash(const QString& filePath) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }
    
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (hash.addData(&file)) {
        return hash.result().toHex();
    }
    
    return QString();
}

bool SubprocessWrapper::setExecutable(const QString& filePath) {
    // Safety check: verify file exists before trying to set permissions
    QFileInfo fileInfo(filePath);
    if (!fileInfo.exists()) {
        qWarning() << "File does not exist for setExecutable:" << filePath;
        return false;
    }
    QFile file(filePath);
    if (!file.exists()) {
        return false;
    }
    
    QFile::Permissions perms = file.permissions();
    perms |= QFile::ExeUser | QFile::ExeGroup | QFile::ExeOther;
    return file.setPermissions(perms);
}

bool SubprocessWrapper::createHardLink(const QString& source, const QString& destination) {
    QFileInfo sourceInfo(source);
    if (!sourceInfo.exists() || !sourceInfo.isFile()) {
        return false;
    }
    
    QFileInfo destInfo(destination);
    QDir destDir = destInfo.dir();
    if (!destDir.exists()) {
        if (!destDir.mkpath(".")) {
            return false;
        }
    }

    if (QFileInfo::exists(destination) || QFileInfo(destination).isSymLink()) {
        QFile::remove(destination);
    }
    
    // Try to create hardlink using link() system call
    // If hardlink fails (e.g., cross-filesystem), fall back to copy
    if (QFile::link(source, destination)) {
        // Check if it's actually a hardlink (not a symlink)
        QFileInfo linkInfo(destination);
        if (linkInfo.isSymLink()) {
            // It created a symlink instead, remove it and copy
            QFile::remove(destination);
            return QFile::copy(source, destination);
        }
        return true;
    }
    
    // If link() fails, fall back to copy
    return QFile::copy(source, destination);
}

QString detectSystemArchitecture() {
    QProcess process;
    process.start("uname", QStringList() << "-m");
    if (!process.waitForFinished(5000)) {
        return "x86_64"; // Default fallback
    }
    QString arch = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    if (arch == "aarch64" || arch == "arm64") {
        return "aarch64";
    } else if (arch == "x86_64") {
        return "x86_64";
    }
    return "x86_64"; // Default fallback
}

QString extractDesktopExecBinary(const QString& execCommand) {
    QString cleaned = execCommand.trimmed();
    cleaned.remove(QRegularExpression("%[fFuUdDnNickvm]"));
    cleaned = cleaned.trimmed();

    const QRegularExpression tokenRegex(R"((\"[^\"]*\"|'[^']*'|\S+))");
    auto it = tokenRegex.globalMatch(cleaned);
    bool skipNext = false;

    while (it.hasNext()) {
        QString token = stripQuotes(it.next().captured(1));
        if (token.isEmpty()) {
            continue;
        }

        if (skipNext) {
            skipNext = false;
            continue;
        }

        if (token == "env") {
            continue;
        }

        if (token == "-S") {
            skipNext = true;
            continue;
        }

        if (token == "-jar") {
            skipNext = true;
            continue;
        }

        if (token.startsWith('-')) {
            continue;
        }

        if (token.contains('=') && !token.startsWith('/') && !token.startsWith("./") && !token.startsWith("../")) {
            continue;
        }

        if (token == "sh" || token == "bash" || token == "/bin/sh" || token == "/bin/bash" ||
            token == "python" || token == "python3" || token == "/usr/bin/python" ||
            token == "/usr/bin/python3" || token == "java" || token == "/usr/bin/java") {
            continue;
        }

        return token;
    }

    return QString();
}

QString resolveExecutableFromCommand(const QString& execCommand, const QStringList& executables) {
    const QRegularExpression jarRegex(R"((\"[^\"]+\.jar\"|'[^']+\.jar'|[^\s]+\.jar))");
    const QRegularExpressionMatch jarMatch = jarRegex.match(execCommand);
    if (jarMatch.hasMatch()) {
        const QString jarToken = stripQuotes(jarMatch.captured(1));
        const QFileInfo jarInfo(jarToken);
        const QString jarName = jarInfo.fileName().toLower();
        for (const QString& exec : executables) {
            const QFileInfo execInfo(exec);
            if (exec == jarToken || exec.endsWith(jarToken) || execInfo.fileName().toLower() == jarName) {
                return exec;
            }
        }
    }

    const QString token = extractDesktopExecBinary(execCommand);
    if (token.isEmpty()) {
        return QString();
    }

    const QFileInfo tokenInfo(token);
    const QString tokenName = tokenInfo.fileName().toLower();
    const QString tokenBaseName = tokenInfo.baseName().toLower();

    for (const QString& exec : executables) {
        const QFileInfo execInfo(exec);
        if (exec == token || exec.endsWith(token)) {
            return exec;
        }

        const QString execName = execInfo.fileName().toLower();
        const QString execBaseName = execInfo.baseName().toLower();
        if (execName == tokenName || execBaseName == tokenBaseName) {
            return exec;
        }
    }

    return QString();
}

QStringList normalizeRpmRequires(const QStringList& rawRequires) {
    QStringList depends;
    for (const QString& rawLine : rawRequires) {
        QString dep = rawLine.trimmed();
        if (dep.isEmpty()) {
            continue;
        }

        // Skip internal/synthetic requirements and file-path dependencies that
        // are not installable packages or resolvable sonames.
        if (dep.startsWith("rpmlib(") ||
            dep.startsWith("config(") ||
            dep.startsWith("rtld(") ||
            dep.startsWith("/")) {
            continue;
        }

        // Strip version constraints expressed inline: "libfoo >= 1.2" -> "libfoo".
        const int spacePos = dep.indexOf(' ');
        if (spacePos > 0) {
            dep = dep.left(spacePos);
        }

        // Strip RPM capability suffixes so soname requirements become real
        // sonames that ldd-based resolution can match:
        //   "libc.so.6()(64bit)"           -> "libc.so.6"
        //   "libc.so.6(GLIBC_2.34)(64bit)" -> "libc.so.6"
        //   "pkgconfig(foo)"               -> dropped (not a package/soname)
        const int parenPos = dep.indexOf('(');
        if (parenPos >= 0) {
            const QString prefix = dep.left(parenPos);
            if (prefix.contains(".so")) {
                dep = prefix;  // soname capability
            } else {
                continue;      // pkgconfig()/perl()/cmake() style capabilities
            }
        }

        dep = dep.trimmed();
        if (!dep.isEmpty() && !depends.contains(dep)) {
            depends.append(dep);
        }
    }

    return depends;
}

ProcessResult SubprocessWrapper::executeWithSudo(const QString& command,
                                                  const QStringList& arguments,
                                                  const QString& password,
                                                  const QString& workingDirectory,
                                                  int timeoutMs) {
    ProcessResult result;
    result.success = false;
    
    QProcess process;
    
    // Build sudo command: echo "password" | sudo -S command args
    QStringList sudoArgs;
    sudoArgs << "-S";  // Read password from stdin
    sudoArgs << command;
    sudoArgs << arguments;
    
    process.setProgram("sudo");
    process.setArguments(sudoArgs);
    
    if (!workingDirectory.isEmpty()) {
        process.setWorkingDirectory(workingDirectory);
    }
    
    process.start();
    
    if (!process.waitForStarted(timeoutMs)) {
        result.errorMessage = QString("Failed to start sudo process: %1").arg(command);
        return result;
    }
    
    // Write password to stdin
    if (!password.isEmpty()) {
        // Wait a bit for sudo to prompt for password (it may write to stderr first)
        QThread::msleep(100);
        
        // Check if process is still running
        if (process.state() != QProcess::Running) {
            result.errorMessage = "Sudo process terminated before password could be sent";
            return result;
        }
        
        // Write password with newline
        QByteArray passwordBytes = password.toUtf8() + "\n";
        qint64 written = process.write(passwordBytes);
        
        if (written != passwordBytes.size()) {
            result.errorMessage = QString("Failed to write password to sudo (wrote %1 of %2 bytes)")
                .arg(written).arg(passwordBytes.size());
            return result;
        }
        
        // Flush and close write channel
        process.waitForBytesWritten(1000);
        process.closeWriteChannel();
    }
    
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(1000);
        result.errorMessage = QString("Sudo process timed out: %1").arg(command);
        return result;
    }
    
    result.exitCode = process.exitCode();
    result.stdoutOutput = QString::fromUtf8(process.readAllStandardOutput());
    result.stderrOutput = QString::fromUtf8(process.readAllStandardError());
    result.success = (process.exitStatus() == QProcess::NormalExit && result.exitCode == 0);
    
    if (!result.success) {
        result.errorMessage = QString("Sudo process failed with exit code %1: %2")
            .arg(result.exitCode)
            .arg(result.stderrOutput.isEmpty() ? result.stdoutOutput : result.stderrOutput);
    }
    
    return result;
}
