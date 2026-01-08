#include "utils.h"
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QCryptographicHash>
#include <QStandardPaths>
#include <QDebug>

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
    
    return QFile::copy(source, destination);
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
    
    // Use QDir::NoSymLinks to avoid copying symlinks
    QStringList files = sourceDir.entryList(QDir::Files | QDir::NoSymLinks);
    for (const QString& file : files) {
        QString srcPath = sourceDir.absoluteFilePath(file);
        QString destPath = destDir.absoluteFilePath(file);
        if (!QFile::copy(srcPath, destPath)) {
            qWarning() << "Failed to copy file:" << srcPath << "to" << destPath;
            // Continue with other files instead of failing completely
        }
    }
    
    QStringList dirs = sourceDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    for (const QString& dir : dirs) {
        QString srcPath = sourceDir.absoluteFilePath(dir);
        QString destPath = destDir.absoluteFilePath(dir);
        if (!copyDirectory(srcPath, destPath)) {
            qWarning() << "Failed to copy subdirectory:" << srcPath;
            // Continue with other directories
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

