#include "size_optimizer.h"
#include "utils.h"
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFile>
#include <QLocale>

SizeOptimizer::SizeOptimizer(QObject* parent)
    : QObject(parent)
{
}

SizeOptimizer::~SizeOptimizer() {
}

void SizeOptimizer::setSettings(const OptimizationSettings& settings) {
    m_settings = settings;
}

qint64 SizeOptimizer::calculateDirSize(const QString& dirPath) {
    qint64 totalSize = 0;
    
    QDirIterator it(dirPath, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        QFileInfo info = it.fileInfo();
        if (info.isFile()) {
            totalSize += info.size();
        }
    }
    
    return totalSize;
}

bool SizeOptimizer::isElfBinary(const QString& filePath) {
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

bool SizeOptimizer::stripFile(const QString& filePath) {
    // Use strip command to remove debug symbols
    ProcessResult result = SubprocessWrapper::execute("strip", {"--strip-unneeded", filePath});
    return result.success;
}

bool SizeOptimizer::stripBinaries(const QString& appDirPath) {
    emit log("Stripping debug symbols from binaries...");
    
    int strippedCount = 0;
    qint64 savedBytes = 0;
    
    QDirIterator it(appDirPath, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        QString filePath = it.next();
        QFileInfo info(filePath);
        
        if (!info.isFile()) continue;
        
        // Skip symlinks
        if (info.isSymLink()) continue;
        
        // Check if it's an ELF binary
        if (isElfBinary(filePath)) {
            qint64 sizeBefore = info.size();
            
            if (stripFile(filePath)) {
                QFileInfo infoAfter(filePath);
                qint64 sizeAfter = infoAfter.size();
                qint64 saved = sizeBefore - sizeAfter;
                
                if (saved > 0) {
                    savedBytes += saved;
                    strippedCount++;
                }
            }
        }
    }
    
    m_report.strippedBytes = savedBytes;
    emit log(QString("  Stripped %1 binaries, saved %2 KB")
        .arg(strippedCount)
        .arg(savedBytes / 1024));
    
    return true;
}

qint64 SizeOptimizer::removeMatchingFiles(const QString& dirPath, const QStringList& patterns) {
    qint64 removedBytes = 0;
    
    for (const QString& pattern : patterns) {
        QDirIterator it(dirPath, {pattern}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            QString filePath = it.next();
            QFileInfo info(filePath);
            qint64 fileSize = info.size();
            
            if (QFile::remove(filePath)) {
                removedBytes += fileSize;
                m_report.removedFiles++;
                m_report.removedPaths.append(filePath);
            }
        }
    }
    
    return removedBytes;
}

qint64 SizeOptimizer::removeDirectories(const QString& basePath, const QStringList& dirNames) {
    qint64 removedBytes = 0;
    
    for (const QString& dirName : dirNames) {
        QDirIterator it(basePath, {dirName}, QDir::Dirs, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            QString dirPath = it.next();
            
            // Calculate size before removal
            qint64 dirSize = calculateDirSize(dirPath);
            
            // Remove directory
            QDir dir(dirPath);
            if (dir.removeRecursively()) {
                removedBytes += dirSize;
                m_report.removedPaths.append(dirPath);
            }
        }
    }
    
    return removedBytes;
}

bool SizeOptimizer::removeStaticLibraries(const QString& appDirPath) {
    emit log("Removing static libraries (*.a)...");
    
    qint64 removed = removeMatchingFiles(appDirPath, {"*.a"});
    m_report.removedBytes += removed;
    
    emit log(QString("  Removed %1 KB of static libraries").arg(removed / 1024));
    return true;
}

bool SizeOptimizer::removeLiToolFiles(const QString& appDirPath) {
    emit log("Removing libtool files (*.la)...");
    
    qint64 removed = removeMatchingFiles(appDirPath, {"*.la"});
    m_report.removedBytes += removed;
    
    emit log(QString("  Removed %1 KB of libtool files").arg(removed / 1024));
    return true;
}

bool SizeOptimizer::removeDocumentation(const QString& appDirPath) {
    emit log("Removing documentation...");
    
    qint64 removed = 0;
    
    // Remove man pages
    removed += removeDirectories(appDirPath, {"man"});
    
    // Remove doc directories
    removed += removeDirectories(appDirPath, {"doc"});
    
    // Remove info directories
    removed += removeDirectories(appDirPath, {"info"});
    
    // Remove help directories (but not application help)
    // removed += removeDirectories(appDirPath, {"help"});
    
    // Remove readme, changelog, license files in lib directories
    removed += removeMatchingFiles(appDirPath + "/usr/lib", {"README*", "CHANGELOG*", "NEWS*", "AUTHORS*"});
    removed += removeMatchingFiles(appDirPath + "/usr/share", {"README*", "CHANGELOG*", "NEWS*", "AUTHORS*"});
    
    m_report.removedBytes += removed;
    
    emit log(QString("  Removed %1 KB of documentation").arg(removed / 1024));
    return true;
}

bool SizeOptimizer::shouldKeepLocale(const QString& localeName) {
    // Always keep system locale
    QString systemLocale = QLocale::system().name();
    if (localeName == systemLocale || localeName.startsWith(systemLocale.split('_').first())) {
        return true;
    }
    
    // Check against keep list
    for (const QString& keep : m_settings.keepLocales) {
        if (localeName == keep || localeName.startsWith(keep + "_")) {
            return true;
        }
    }
    
    return false;
}

bool SizeOptimizer::removeUnneededLocales(const QString& appDirPath) {
    emit log("Removing unneeded locales...");
    
    qint64 removed = 0;
    
    // Find locale directories
    QStringList localeDirs = {
        appDirPath + "/usr/share/locale",
        appDirPath + "/usr/lib/locale"
    };
    
    for (const QString& localeDir : localeDirs) {
        QDir dir(localeDir);
        if (!dir.exists()) continue;
        
        QStringList entries = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString& entry : entries) {
            if (!shouldKeepLocale(entry)) {
                QString fullPath = dir.absoluteFilePath(entry);
                qint64 dirSize = calculateDirSize(fullPath);
                
                QDir removeDir(fullPath);
                if (removeDir.removeRecursively()) {
                    removed += dirSize;
                    m_report.removedPaths.append(fullPath);
                }
            }
        }
    }
    
    // Also clean up locales directory in electron apps
    QDirIterator it(appDirPath, {"locales"}, QDir::Dirs, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        QString localesDir = it.next();
        QDir dir(localesDir);
        
        // Electron locales are .pak files
        QStringList pakFiles = dir.entryList({"*.pak"}, QDir::Files);
        for (const QString& pakFile : pakFiles) {
            QString localeName = pakFile.section('.', 0, 0);  // e.g., "en-US" from "en-US.pak"
            localeName.replace('-', '_');  // Normalize to underscore format
            
            if (!shouldKeepLocale(localeName) && 
                !shouldKeepLocale(localeName.split('_').first())) {
                QString fullPath = dir.absoluteFilePath(pakFile);
                QFileInfo info(fullPath);
                qint64 fileSize = info.size();
                
                if (QFile::remove(fullPath)) {
                    removed += fileSize;
                    m_report.removedFiles++;
                }
            }
        }
    }
    
    m_report.removedBytes += removed;
    
    emit log(QString("  Removed %1 KB of unneeded locales").arg(removed / 1024));
    return true;
}

bool SizeOptimizer::removeHeaderFiles(const QString& appDirPath) {
    emit log("Removing header files...");
    
    qint64 removed = 0;
    
    // Remove include directories
    removed += removeDirectories(appDirPath, {"include"});
    
    // Remove header files elsewhere
    removed += removeMatchingFiles(appDirPath, {"*.h", "*.hpp", "*.hxx"});
    
    m_report.removedBytes += removed;
    
    emit log(QString("  Removed %1 KB of header files").arg(removed / 1024));
    return true;
}

bool SizeOptimizer::removePkgConfigFiles(const QString& appDirPath) {
    emit log("Removing pkg-config files...");
    
    qint64 removed = 0;
    
    // Remove pkgconfig directories
    removed += removeDirectories(appDirPath, {"pkgconfig"});
    
    // Remove .pc files elsewhere
    removed += removeMatchingFiles(appDirPath, {"*.pc"});
    
    m_report.removedBytes += removed;
    
    emit log(QString("  Removed %1 KB of pkg-config files").arg(removed / 1024));
    return true;
}

bool SizeOptimizer::optimizeAppDir(const QString& appDirPath) {
    if (!m_settings.enabled) {
        emit log("Size optimization is disabled");
        return true;
    }
    
    emit log("=== Starting size optimization ===");
    
    // Calculate original size
    m_report = SizeReport();
    m_report.originalSize = calculateDirSize(appDirPath);
    
    emit log(QString("Original AppDir size: %1 MB")
        .arg(m_report.originalSize / 1024.0 / 1024.0, 0, 'f', 2));
    
    int step = 0;
    int totalSteps = 7;  // Number of optimization steps
    
    // Step 1: Strip binaries
    if (m_settings.stripBinaries) {
        emit progress(++step * 100 / totalSteps, "Stripping binaries...");
        stripBinaries(appDirPath);
    }
    
    // Step 2: Remove static libraries
    if (m_settings.removeStaticLibs) {
        emit progress(++step * 100 / totalSteps, "Removing static libraries...");
        removeStaticLibraries(appDirPath);
    }
    
    // Step 3: Remove libtool files
    if (m_settings.removeLiToolLibs) {
        emit progress(++step * 100 / totalSteps, "Removing libtool files...");
        removeLiToolFiles(appDirPath);
    }
    
    // Step 4: Remove documentation
    if (m_settings.removeDocumentation) {
        emit progress(++step * 100 / totalSteps, "Removing documentation...");
        removeDocumentation(appDirPath);
    }
    
    // Step 5: Remove unneeded locales
    if (m_settings.removeUnneededLocales) {
        emit progress(++step * 100 / totalSteps, "Removing unneeded locales...");
        removeUnneededLocales(appDirPath);
    }
    
    // Step 6: Remove header files
    if (m_settings.removeHeaderFiles) {
        emit progress(++step * 100 / totalSteps, "Removing header files...");
        removeHeaderFiles(appDirPath);
    }
    
    // Step 7: Remove pkg-config files
    if (m_settings.removePkgConfig) {
        emit progress(++step * 100 / totalSteps, "Removing pkg-config files...");
        removePkgConfigFiles(appDirPath);
    }
    
    // Calculate final size
    m_report.optimizedSize = calculateDirSize(appDirPath);
    
    emit log("=== Size optimization complete ===");
    emit log(m_report.summary());
    emit log(QString("  Total files removed: %1").arg(m_report.removedFiles));
    emit log(QString("  Bytes stripped from binaries: %1 KB").arg(m_report.strippedBytes / 1024));
    emit log(QString("  Bytes from removed files: %1 KB").arg(m_report.removedBytes / 1024));
    
    return true;
}

QStringList SizeOptimizer::getCompressionArgs() const {
    QStringList args;
    
    switch (m_settings.compression) {
        case CompressionLevel::FAST:
            args << "--comp" << "gzip" << "-Xcompression-level" << "1";
            break;
        case CompressionLevel::NORMAL:
            // Default gzip compression
            args << "--comp" << "gzip";
            break;
        case CompressionLevel::MAXIMUM:
            args << "--comp" << "zstd" << "-Xcompression-level" << "19";
            break;
        case CompressionLevel::ULTRA:
            args << "--comp" << "zstd" << "-Xcompression-level" << "22";
            break;
    }
    
    return args;
}


