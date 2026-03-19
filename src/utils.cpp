#include "utils.h"
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QCryptographicHash>
#include <QStandardPaths>
#include <QThread>
#include <QDebug>
#include <QRegularExpression>

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

bool recreateSymlink(const QFileInfo& sourceInfo, const QString& destination) {
    const QString target = sourceInfo.symLinkTarget();
    if (target.isEmpty()) {
        return false;
    }

    QFile::remove(destination);
    return QFile::link(target, destination);
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
        result.errorMessage = QString("Process timed out: %1").arg(command);
        return result;
    }
    
    result.exitCode = process.exitCode();
    result.stdoutOutput = QString::fromUtf8(process.readAllStandardOutput());
    result.stderrOutput = QString::fromUtf8(process.readAllStandardError());
    result.success = (result.exitCode == 0);
    
    if (!result.success) {
        result.errorMessage = QString("Process failed with exit code %1: %2")
            .arg(result.exitCode)
            .arg(result.stderrOutput.isEmpty() ? result.stdoutOutput : result.stderrOutput);
    }
    
    return result;
}

bool SubprocessWrapper::copyFile(const QString& source, const QString& destination) {
    QFileInfo sourceInfo(source);
    if (!sourceInfo.exists()) {
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
        return recreateSymlink(sourceInfo, destination);
    }

    if (!QFile::copy(source, destination)) {
        return false;
    }

    QFile(destination).setPermissions(sourceInfo.permissions());
    return true;
}

bool SubprocessWrapper::copyDirectory(const QString& source, const QString& destination) {
    QDir sourceDir(source);
    if (!sourceDir.exists()) {
        qWarning() << "Source directory does not exist:" << source;
        return false;
    }
    
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
            if (!copyFile(srcPath, destPath)) {
                qWarning() << "Failed to copy symlink:" << srcPath << "to" << destPath;
            }
            continue;
        }

        if (entry.isDir()) {
            if (!copyDirectory(srcPath, destPath)) {
                qWarning() << "Failed to copy subdirectory:" << srcPath;
            }
            continue;
        }

        if (!copyFile(srcPath, destPath)) {
            qWarning() << "Failed to copy file:" << srcPath << "to" << destPath;
        }
    }
    
    return true;
}

bool SubprocessWrapper::createDirectory(const QString& path) {
    QDir dir;
    return dir.mkpath(path);
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
        result.errorMessage = QString("Sudo process timed out: %1").arg(command);
        return result;
    }
    
    result.exitCode = process.exitCode();
    result.stdoutOutput = QString::fromUtf8(process.readAllStandardOutput());
    result.stderrOutput = QString::fromUtf8(process.readAllStandardError());
    result.success = (result.exitCode == 0);
    
    if (!result.success) {
        result.errorMessage = QString("Sudo process failed with exit code %1: %2")
            .arg(result.exitCode)
            .arg(result.stderrOutput.isEmpty() ? result.stdoutOutput : result.stderrOutput);
    }
    
    return result;
}
