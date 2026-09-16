#include "dependencyanalyzer.h"
#include "utils.h"
#include "cache_manager.h"
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
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

QStringList DependencyAnalyzer::runLdd(const QString& executablePath, const QProcessEnvironment& env) {
    // Check cache first (only if default/empty environment is used)
    QString binaryHash = CacheManager::calculateBinaryHash(executablePath);
    if (!binaryHash.isEmpty() && env.isEmpty()) {
        QStringList cachedResult = CacheManager::getLddCache(binaryHash);
        if (!cachedResult.isEmpty()) {
            return cachedResult;
        }
    }
    
    // Try to use bubblewrap sandbox (--unshare-all), fallback to readelf -d passive parsing.
    // Untrusted binaries are NEVER executed unsheltered.
    QStringList lddOutput;
    const QString absBinaryPath = QFileInfo(executablePath).absoluteFilePath();
    
    const bool bwrapBypassed = qEnvironmentVariableIsSet("APPALCHEMIST_DISABLE_BWRAP") ||
                               env.contains("APPALCHEMIST_DISABLE_BWRAP");
    const QString bwrapPath = bwrapBypassed ? QString() : QStandardPaths::findExecutable("bwrap");

    if (!bwrapPath.isEmpty()) {
        // Use bubblewrap with full sandboxing (unshare all, read-only root, dev, proc, tmpfs)
        QStringList bwrapArgs = {
            "--unshare-all",
            "--ro-bind", "/", "/",
            "--dev", "/dev",
            "--proc", "/proc",
            "--tmpfs", "/tmp"
        };

        // If target binary is under /tmp/ or /var/tmp/, bind-mount it read-only
        // to prevent --tmpfs /tmp from masking the binary
        if (absBinaryPath.startsWith("/tmp/") || absBinaryPath.startsWith("/var/tmp/")) {
            bwrapArgs << "--ro-bind" << absBinaryPath << absBinaryPath;
        }

        // If an enclosing directory (such as AppDir or extracted tree) is known under /tmp/ or /var/tmp/, bind-mount it
        QDir dir(QFileInfo(absBinaryPath).absolutePath());
        QString enclosingDir;
        while (!dir.isRoot()) {
            const QString path = dir.absolutePath();
            const QString name = dir.dirName();
            if (name.endsWith(".AppDir", Qt::CaseInsensitive) || name == "AppDir" ||
                path.contains("/appalchemist-") || path.contains("/staged_deps")) {
                enclosingDir = path;
                break;
            }
            if (!dir.cdUp()) break;
        }
        if (!enclosingDir.isEmpty() && (enclosingDir.startsWith("/tmp/") || enclosingDir.startsWith("/var/tmp/"))) {
            bwrapArgs << "--ro-bind" << enclosingDir << enclosingDir;
        }

        // If env contains LD_LIBRARY_PATH, split by ':' and for each directory path located
        // under /tmp/ or /var/tmp/ that exists on disk, bind-mount it read-only
        QString ldLibraryPath = env.value("LD_LIBRARY_PATH");
        if (ldLibraryPath.isEmpty()) {
            ldLibraryPath = QProcessEnvironment::systemEnvironment().value("LD_LIBRARY_PATH");
        }
        if (!ldLibraryPath.isEmpty()) {
            const QStringList libDirs = ldLibraryPath.split(':', Qt::SkipEmptyParts);
            for (const QString& dirPath : libDirs) {
                const QString absDirPath = QFileInfo(dirPath).absoluteFilePath();
                if ((absDirPath.startsWith("/tmp/") || absDirPath.startsWith("/var/tmp/")) && QDir(absDirPath).exists()) {
                    bwrapArgs << "--ro-bind" << absDirPath << absDirPath;
                }
            }
        }

        bwrapArgs << "--" << "ldd" << absBinaryPath;

        const bool simulateFailure = qEnvironmentVariableIsSet("APPALCHEMIST_SIMULATE_BWRAP_FAILURE") ||
                                     env.contains("APPALCHEMIST_SIMULATE_BWRAP_FAILURE");
        if (!simulateFailure) {
            ProcessResult result = SubprocessWrapper::execute(bwrapPath, bwrapArgs, {}, 5000, env);
            if (result.success && !result.stdoutOutput.trimmed().isEmpty()) {
                lddOutput = result.stdoutOutput.split('\n', Qt::SkipEmptyParts);
            }
        }
    }

    // Automatic fallback to passive DT_NEEDED parsing via readelf -d (never executes binary code):
    // Triggers if bwrap is missing, bypassed, or if bwrap execution failed (e.g. exit code != 0,
    // empty output, or unprivileged user namespaces disabled)
    if (lddOutput.isEmpty()) {
        QString readelfCmd = QStandardPaths::findExecutable("readelf");
        if (readelfCmd.isEmpty()) {
            readelfCmd = QStandardPaths::findExecutable("llvm-readelf");
        }
        if (!readelfCmd.isEmpty()) {
            QProcessEnvironment readelfEnv = env;
            readelfEnv.insert("LC_ALL", "C");
            ProcessResult result = SubprocessWrapper::execute(readelfCmd, {"-d", absBinaryPath}, {}, 5000, readelfEnv);
            if (result.success) {
                QStringList searchDirs;
                QString ldLibraryPath = env.value("LD_LIBRARY_PATH");
                if (ldLibraryPath.isEmpty()) {
                    ldLibraryPath = QProcessEnvironment::systemEnvironment().value("LD_LIBRARY_PATH");
                }
                if (!ldLibraryPath.isEmpty()) {
                    searchDirs << ldLibraryPath.split(':', Qt::SkipEmptyParts);
                }
                const QString binDir = QFileInfo(absBinaryPath).absolutePath();
                if (!binDir.isEmpty()) {
                    searchDirs << binDir;
                    searchDirs << QDir(binDir).filePath("../lib");
                    searchDirs << QDir(binDir).filePath("../lib64");
                }
                searchDirs << "/usr/lib/x86_64-linux-gnu" << "/lib/x86_64-linux-gnu"
                           << "/usr/lib/aarch64-linux-gnu" << "/lib/aarch64-linux-gnu"
                           << "/usr/lib64" << "/lib64" << "/usr/lib" << "/lib";

                const QStringList rawLines = result.stdoutOutput.split('\n', Qt::SkipEmptyParts);
                for (const QString& rLine : rawLines) {
                    if (rLine.contains("(RPATH)") || rLine.contains("(RUNPATH)")) {
                        const int openBracket = rLine.indexOf('[');
                        const int closeBracket = rLine.indexOf(']', openBracket);
                        if (openBracket >= 0 && closeBracket > openBracket) {
                            QString rpathStr = rLine.mid(openBracket + 1, closeBracket - openBracket - 1).trimmed();
                            rpathStr.replace("$ORIGIN", binDir);
                            rpathStr.replace("${ORIGIN}", binDir);
                            const QStringList rpathDirs = rpathStr.split(':', Qt::SkipEmptyParts);
                            for (int i = rpathDirs.size() - 1; i >= 0; --i) {
                                searchDirs.prepend(rpathDirs[i]);
                            }
                        }
                    }
                }

                for (const QString& rLine : rawLines) {
                    if (!rLine.contains("(NEEDED)")) continue;
                    const int openBracket = rLine.indexOf('[');
                    const int closeBracket = rLine.indexOf(']', openBracket);
                    if (openBracket < 0 || closeBracket <= openBracket) continue;
                    const QString soname = rLine.mid(openBracket + 1, closeBracket - openBracket - 1).trimmed();
                    if (soname.isEmpty()) continue;

                    QString resolvedPath;
                    for (const QString& sDir : searchDirs) {
                        const QString candidate = sDir + "/" + soname;
                        if (QFileInfo::exists(candidate)) {
                            resolvedPath = candidate;
                            break;
                        }
                    }
                    if (!resolvedPath.isEmpty()) {
                        lddOutput.append(QString("%1 => %2 (0x0)").arg(soname, resolvedPath));
                    } else {
                        lddOutput.append(QString("%1 => not found").arg(soname));
                    }
                }
            }
        }
    }
    
    // Cache the result (only when using default/empty environment)
    if (!binaryHash.isEmpty() && !lddOutput.isEmpty() && env.isEmpty()) {
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





