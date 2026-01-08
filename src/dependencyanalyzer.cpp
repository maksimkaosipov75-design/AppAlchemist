#include "dependencyanalyzer.h"
#include "utils.h"
#include "cache_manager.h"
#include <QFileInfo>
#include <QRegularExpression>
#include <QDebug>

DependencyAnalyzer::DependencyAnalyzer() {
    initializeSystemPatterns();
}

void DependencyAnalyzer::initializeSystemPatterns() {
    // System libraries that should not be bundled
    m_systemLibraryPatterns = {
        "libc.so",
        "libstdc++",
        "libgcc_s",
        "libpthread",
        "libdl.so",
        "libm.so",
        "librt.so",
        "libresolv.so",
        "libnss_",
        "libselinux",
        "libcrypt",
        "libpcre",
        "libz.so",
        "libbz2.so",
        "liblzma.so",
        "libssl.so",
        "libcrypto.so",
        "/lib64/ld-linux",
        "/lib/ld-linux"
    };
    
    // System packages that should not be bundled
    m_systemPackagePatterns = {
        "systemd",
        "dbus",
        "udev",
        "libc6",
        "libstdc++",
        "libgcc",
        "libpthread",
        "libselinux"
    };
}

QList<LibraryInfo> DependencyAnalyzer::analyzeExecutable(const QString& executablePath) {
    QList<LibraryInfo> libraries;
    
    QStringList lddOutput = runLdd(executablePath);
    
    for (const QString& line : lddOutput) {
        if (line.contains("=>")) {
            // Parse ldd output: libname => /path/to/lib.so
            QRegularExpression re(R"((\S+)\s*=>\s*(\S+))");
            QRegularExpressionMatch match = re.match(line);
            
            if (match.hasMatch()) {
                LibraryInfo info;
                info.name = match.captured(1);
                info.path = match.captured(2);
                info.isSystemLibrary = isSystemLibrary(info.path);
                
                if (!info.isSystemLibrary && QFileInfo::exists(info.path)) {
                    libraries.append(info);
                }
            }
        } else if (line.trimmed().startsWith("/")) {
            // Direct path without => (statically linked or absolute path)
            QString libPath = line.trimmed().split(' ').first();
            if (QFileInfo::exists(libPath) && !isSystemLibrary(libPath)) {
                LibraryInfo info;
                info.path = libPath;
                info.name = QFileInfo(libPath).fileName();
                info.isSystemLibrary = false;
                libraries.append(info);
            }
        }
    }
    
    return libraries;
}

QStringList DependencyAnalyzer::runLdd(const QString& executablePath) {
    // Check cache first
    QString binaryHash = CacheManager::calculateBinaryHash(executablePath);
    if (!binaryHash.isEmpty()) {
        QStringList cachedResult = CacheManager::getLddCache(binaryHash);
        if (!cachedResult.isEmpty()) {
            return cachedResult;
        }
    }
    
    // Try to use firejail for sandboxing, fallback to bubblewrap, then direct ldd
    QStringList lddOutput;
    
    // Check for firejail first
    ProcessResult firejailCheck = SubprocessWrapper::execute("which", {"firejail"}, {}, 1000);
    bool hasFirejail = firejailCheck.success && !firejailCheck.stdoutOutput.trimmed().isEmpty();
    
    if (hasFirejail) {
        // Use firejail to sandbox ldd
        ProcessResult result = SubprocessWrapper::execute("firejail", {
            "--quiet", "--", "ldd", executablePath
        }, {}, 5000);
        
        if (result.success) {
            lddOutput = result.stdoutOutput.split('\n', Qt::SkipEmptyParts);
        }
    } else {
        // Check for bubblewrap
        ProcessResult bwrapCheck = SubprocessWrapper::execute("which", {"bwrap"}, {}, 1000);
        bool hasBwrap = bwrapCheck.success && !bwrapCheck.stdoutOutput.trimmed().isEmpty();
        
        if (hasBwrap) {
            // Use bubblewrap to sandbox ldd
            ProcessResult result = SubprocessWrapper::execute("bwrap", {
                "--ro-bind", "/", "/",
                "--dev", "/dev",
                "--proc", "/proc",
                "--tmpfs", "/tmp",
                "--", "ldd", executablePath
            }, {}, 5000);
            
            if (result.success) {
                lddOutput = result.stdoutOutput.split('\n', Qt::SkipEmptyParts);
            }
        } else {
            // Fallback to direct ldd (less secure but functional)
            ProcessResult result = SubprocessWrapper::execute("ldd", {executablePath}, {}, 5000);
            
            if (result.success) {
                lddOutput = result.stdoutOutput.split('\n', Qt::SkipEmptyParts);
            }
        }
    }
    
    // Cache the result
    if (!binaryHash.isEmpty() && !lddOutput.isEmpty()) {
        CacheManager::setLddCache(binaryHash, lddOutput);
    }
    
    return lddOutput;
}

QStringList DependencyAnalyzer::collectLibraries(const QStringList& executables) {
    QSet<QString> uniqueLibraries;
    
    for (const QString& exec : executables) {
        QList<LibraryInfo> libs = analyzeExecutable(exec);
        for (const LibraryInfo& lib : libs) {
            if (QFileInfo::exists(lib.path)) {
                uniqueLibraries.insert(lib.path);
            }
        }
    }
    
    return uniqueLibraries.values();
}

QStringList DependencyAnalyzer::filterSystemLibraries(const QStringList& libraries) {
    QStringList filtered;
    
    for (const QString& lib : libraries) {
        if (!isSystemLibrary(lib)) {
            filtered.append(lib);
        }
    }
    
    return filtered;
}

bool DependencyAnalyzer::isSystemLibrary(const QString& libraryPath) {
    QString libName = QFileInfo(libraryPath).fileName();
    
    for (const QString& pattern : m_systemLibraryPatterns) {
        if (libName.contains(pattern) || libraryPath.contains(pattern)) {
            return true;
        }
    }
    
    // Check if it's in system directories
    if (libraryPath.startsWith("/lib/") || 
        libraryPath.startsWith("/lib64/") ||
        libraryPath.startsWith("/usr/lib/") ||
        libraryPath.startsWith("/usr/lib64/")) {
        // Additional check: if it's a system library pattern
        for (const QString& pattern : m_systemLibraryPatterns) {
            if (libraryPath.contains(pattern)) {
                return true;
            }
        }
    }
    
    return false;
}

QStringList DependencyAnalyzer::checkSystemDependencies(const QStringList& packageDepends) {
    QStringList warnings;
    
    for (const QString& dep : packageDepends) {
        QString depLower = dep.toLower();
        for (const QString& pattern : m_systemPackagePatterns) {
            if (depLower.contains(pattern.toLower())) {
                warnings.append(QString("Warning: Package depends on system package '%1', which may not be available on all distributions")
                    .arg(dep));
                break;
            }
        }
    }
    
    return warnings;
}





