#include "dependency_resolver.h"
#include "utils.h"
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QTextStream>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QEventLoop>
#include <QTimer>
#include <QMap>

DependencyResolver::DependencyResolver(QObject* parent)
    : QObject(parent)
    , m_browser(new RepositoryBrowser(this))
{
    initializeExcludePatterns();
    
    connect(m_browser, &RepositoryBrowser::log, this, &DependencyResolver::log);
}

DependencyResolver::~DependencyResolver() {
}

void DependencyResolver::initializeExcludePatterns() {
    // Core system libraries that should not be bundled
    m_excludePatterns = {
        // Core C library and runtime
        "libc6", "libc-bin", "libc.so", "libm.so", "libdl.so", "librt.so",
        "libpthread.so", "ld-linux", "linux-vdso",
        
        // GCC runtime
        "libgcc", "libstdc++", "libgomp",
        
        // X11 and display
        "libx11", "libxext", "libxrender", "libxrandr", "libxi", "libxcursor",
        "libxcomposite", "libxdamage", "libxfixes", "libxinerama",
        "libdrm", "libgl", "libegl", "libgbm", "libvulkan",
        "libwayland", "libxkb",
        
        // Mesa and graphics drivers
        "mesa", "nvidia", "amdgpu", "radeon", "intel",
        
        // D-Bus and system services
        "libdbus", "libsystemd", "libudev", "libpolkit",
        
        // GLib and GTK base (usually system-provided)
        "libglib-2.0", "libgobject-2.0", "libgio-2.0",
        
        // Audio (system-specific)
        "libasound", "libpulse", "pipewire", "libjack",
        
        // Core utilities
        "coreutils", "base-files", "bash", "dash",
        
        // Font config
        "libfontconfig", "libfreetype",
        
        // Network (system-specific)
        "libssl", "libcrypto", "ca-certificates",
        
        // Kernel modules
        "linux-image", "linux-headers"
    };
    
    // Add patterns from settings
    for (const QString& pattern : m_settings.excludePatterns) {
        m_excludePatterns.insert(pattern.toLower());
    }
}

void DependencyResolver::setSettings(const DependencySettings& settings) {
    m_settings = settings;
    initializeExcludePatterns();
}

bool DependencyResolver::shouldExclude(const QString& name) {
    if (!m_settings.excludeSystemLibs) {
        return false;
    }
    
    QString lowerName = name.toLower();
    
    for (const QString& pattern : m_excludePatterns) {
        if (lowerName.contains(pattern)) {
            return true;
        }
    }
    
    return false;
}

QStringList DependencyResolver::parseDependencies(const QString& controlFilePath) {
    QStringList depends;
    
    QFile file(controlFilePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        emit log(QString("ERROR: Cannot open control file: %1").arg(controlFilePath));
        return depends;
    }
    
    QTextStream in(&file);
    QString currentField;
    QString currentValue;
    
    while (!in.atEnd()) {
        QString line = in.readLine();
        
        if (line.isEmpty()) continue;
        
        if (line.startsWith(" ") || line.startsWith("\t")) {
            // Continuation line
            currentValue += " " + line.trimmed();
        } else {
            // Process previous field
            if (currentField == "Depends" || currentField == "Pre-Depends") {
                depends.append(parseDependsString(currentValue));
            } else if (currentField == "Recommends" && m_settings.includeRecommended) {
                depends.append(parseDependsString(currentValue));
            }
            
            // New field
            int colonPos = line.indexOf(':');
            if (colonPos > 0) {
                currentField = line.left(colonPos).trimmed();
                currentValue = line.mid(colonPos + 1).trimmed();
            }
        }
    }
    
    // Handle last field
    if (currentField == "Depends" || currentField == "Pre-Depends") {
        depends.append(parseDependsString(currentValue));
    } else if (currentField == "Recommends" && m_settings.includeRecommended) {
        depends.append(parseDependsString(currentValue));
    }
    
    file.close();
    
    // Remove duplicates and excluded packages
    QStringList filtered;
    QSet<QString> seen;
    
    for (const QString& dep : depends) {
        QString baseName = dep.split(' ').first().trimmed();
        if (!shouldExclude(baseName) && !seen.contains(baseName)) {
            filtered.append(baseName);
            seen.insert(baseName);
        }
    }
    
    return filtered;
}

QStringList DependencyResolver::parseDependsString(const QString& dependsStr) {
    QStringList result;
    
    if (dependsStr.isEmpty()) {
        return result;
    }
    
    // Dependencies are comma-separated
    QStringList deps = dependsStr.split(',');
    
    for (QString dep : deps) {
        dep = dep.trimmed();
        if (dep.isEmpty()) continue;
        
        // Handle alternatives (pkg1 | pkg2)
        if (dep.contains('|')) {
            QStringList alternatives = dep.split('|');
            // Take first alternative
            dep = alternatives.first().trimmed();
        }
        
        // Remove version constraints for the result
        QString name, version, op;
        parseVersionConstraint(dep, name, version, op);
        
        if (!name.isEmpty()) {
            result.append(name);
        }
    }
    
    return result;
}

void DependencyResolver::parseVersionConstraint(const QString& dep, QString& name, QString& version, QString& op) {
    // Format: package (>> 1.0) or package (>= 1.0) or package (= 1.0) etc.
    QRegularExpression regex(R"(^([^\s(]+)\s*(?:\(([<>=!]+)\s*([^\s)]+)\))?$)");
    QRegularExpressionMatch match = regex.match(dep.trimmed());
    
    if (match.hasMatch()) {
        name = match.captured(1).trimmed();
        op = match.captured(2).trimmed();
        version = match.captured(3).trimmed();
    } else {
        name = dep.trimmed();
        op.clear();
        version.clear();
    }
}

QString DependencyResolver::findSystemLibrary(const QString& libName) {
    // Common library search paths
    QStringList searchPaths = {
        "/usr/lib",
        "/usr/lib64",
        "/usr/lib/x86_64-linux-gnu",
        "/usr/lib/aarch64-linux-gnu",
        "/lib",
        "/lib64",
        "/lib/x86_64-linux-gnu"
    };
    
    for (const QString& path : searchPaths) {
        QDir dir(path);
        if (!dir.exists()) continue;
        
        // Search for library files
        QStringList patterns = {
            QString("lib%1.so*").arg(libName),
            QString("%1.so*").arg(libName),
            QString("lib%1-*.so*").arg(libName)
        };
        
        for (const QString& pattern : patterns) {
            QStringList files = dir.entryList({pattern}, QDir::Files);
            if (!files.isEmpty()) {
                return dir.absoluteFilePath(files.first());
            }
        }
    }
    
    return QString();
}

QList<ResolvedDependency> DependencyResolver::resolveDependencies(const QStringList& depends, const QString& outputDir) {
    QList<ResolvedDependency> results;
    m_resolvedLibraries.clear();
    
    if (!m_settings.enabled || depends.isEmpty()) {
        return results;
    }
    
    emit resolveStarted();
    emit log(QString("=== Resolving %1 dependencies ===").arg(depends.size()));
    
    int resolved = 0;
    int failed = 0;
    
    for (int i = 0; i < depends.size(); ++i) {
        const QString& dep = depends[i];
        emit progress(i + 1, depends.size());
        
        ResolvedDependency result;
        result.name = dep;
        
        // Check if it should be excluded
        if (shouldExclude(dep)) {
            emit log(QString("  Skipping system package: %1").arg(dep));
            result.isResolved = true;
            result.resolvedPath = "(system)";
            results.append(result);
            continue;
        }
        
        // Try to find on system first
        QString systemLib = findSystemLibrary(dep);
        if (!systemLib.isEmpty()) {
            emit log(QString("  Found on system: %1 -> %2").arg(dep).arg(systemLib));
            result.isResolved = true;
            result.resolvedPath = systemLib;
            m_resolvedLibraries.append(systemLib);
            resolved++;
            results.append(result);
            continue;
        }
        
        // Download if enabled
        if (m_settings.downloadMissing) {
            emit downloadStarted(dep);
            
            if (downloadAndExtract(dep, outputDir)) {
                result.isResolved = true;
                result.resolvedPath = outputDir;
                resolved++;
                emit downloadCompleted(dep, outputDir);
            } else {
                result.isResolved = false;
                result.error = "Download/extraction failed";
                failed++;
                emit downloadError(dep, result.error);
            }
        } else {
            result.isResolved = false;
            result.error = "Not found on system";
            failed++;
        }
        
        results.append(result);
    }
    
    emit log(QString("=== Dependency resolution complete: %1 resolved, %2 failed ===")
        .arg(resolved).arg(failed));
    emit resolveCompleted(resolved, failed);
    
    return results;
}

bool DependencyResolver::downloadAndExtract(const QString& packageName, const QString& outputDir) {
    emit log(QString("  Downloading: %1").arg(packageName));
    
    // Create temp directory for download
    QString tempDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation) 
        + "/appalchemist-deps-" + QString::number(QDateTime::currentMSecsSinceEpoch());
    QDir().mkpath(tempDir);
    
    // Use package manager to download
    PackageManager pm = RepositoryBrowser::detectPackageManager();
    ProcessResult result;
    
    switch (pm) {
        case PackageManager::APT:
            result = SubprocessWrapper::execute("apt-get", 
                {"download", packageName}, tempDir, 120000);
            break;
        case PackageManager::DNF:
            result = SubprocessWrapper::execute("dnf", 
                {"download", "--destdir", tempDir, packageName}, {}, 120000);
            break;
        case PackageManager::PACMAN:
            if (!m_sudoPassword.isEmpty()) {
                // First sync file database if needed
                SubprocessWrapper::executeWithSudo("pacman", {"-Fy"}, m_sudoPassword, {}, 60000);
                // Download package to cache
                result = SubprocessWrapper::executeWithSudo("pacman",
                    {"-Syw", "--noconfirm", packageName}, m_sudoPassword, {}, 120000);
                if (result.success) {
                    // Copy from cache to tempDir (may need sudo)
                    QDir cacheDir("/var/cache/pacman/pkg");
                    QStringList pkgFiles = cacheDir.entryList({QString("%1-*.pkg.tar*").arg(packageName)}, QDir::Files, QDir::Time);
                    if (!pkgFiles.isEmpty()) {
                        QString cachePath = cacheDir.absoluteFilePath(pkgFiles.first());
                        QString destPath = tempDir + "/" + pkgFiles.first();
                        emit log(QString("  Found package in cache: %1").arg(cachePath));
                        // Try direct copy first
                        if (!QFile::copy(cachePath, destPath)) {
                            // If failed, try with sudo
                            emit log(QString("  Direct copy failed, trying with sudo..."));
                            ProcessResult copyResult = SubprocessWrapper::executeWithSudo("cp",
                                {cachePath, destPath}, m_sudoPassword, {}, 30000);
                            if (!copyResult.success) {
                                emit log(QString("  Failed to copy package file: %1").arg(copyResult.stderrOutput));
                                SubprocessWrapper::removeDirectory(tempDir);
                                return false;
                            }
                        }
                        emit log(QString("  Package copied to: %1").arg(destPath));
                    } else {
                        emit log(QString("  Package not found in cache after download"));
                        SubprocessWrapper::removeDirectory(tempDir);
                        return false;
                    }
                } else {
                    emit log(QString("  Failed to download package: %1").arg(result.stderrOutput.left(200)));
                    SubprocessWrapper::removeDirectory(tempDir);
                    return false;
                }
            } else {
                emit log(QString("  Sudo password required for pacman download"));
                SubprocessWrapper::removeDirectory(tempDir);
                return false;
            }
            break;
        default:
            emit log(QString("  Unsupported package manager"));
            SubprocessWrapper::removeDirectory(tempDir);
            return false;
    }
    
    if (!result.success) {
        emit log(QString("  Download failed: %1").arg(result.stderrOutput.left(200)));
        SubprocessWrapper::removeDirectory(tempDir);
        return false;
    }
    
    // Find downloaded package
    QDir dir(tempDir);
    QStringList packages;
    
    if (pm == PackageManager::APT) {
        packages = dir.entryList({"*.deb"}, QDir::Files);
    } else if (pm == PackageManager::DNF) {
        packages = dir.entryList({"*.rpm"}, QDir::Files);
    } else if (pm == PackageManager::PACMAN) {
        packages = dir.entryList({"*.pkg.tar.*", "*.pkg.tar"}, QDir::Files);
    }
    
    if (packages.isEmpty()) {
        emit log(QString("  No package file found after download"));
        SubprocessWrapper::removeDirectory(tempDir);
        return false;
    }
    
    QString packagePath = dir.absoluteFilePath(packages.first());
    emit log(QString("  Downloaded: %1").arg(packagePath));
    
    // Extract libraries
    QStringList libs = extractLibraries(packagePath, outputDir);
    
    if (!libs.isEmpty()) {
        emit log(QString("  Extracted %1 libraries").arg(libs.size()));
        for (const QString& lib : libs) {
            if (QFileInfo::exists(lib)) {
                emit log(QString("    Verified: %1").arg(lib));
            } else {
                emit log(QString("    WARNING: Library not found: %1").arg(lib));
            }
        }
        m_resolvedLibraries.append(libs);
    } else {
        emit log(QString("  WARNING: No libraries extracted from package"));
    }
    
    // Cleanup
    SubprocessWrapper::removeDirectory(tempDir);
    
    return !libs.isEmpty();
}

QStringList DependencyResolver::extractLibraries(const QString& packagePath, const QString& outputDir) {
    QStringList libs;
    
    QString tempExtract = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
        + "/appalchemist-extract-" + QString::number(QDateTime::currentMSecsSinceEpoch());
    QDir().mkpath(tempExtract);
    
    bool extracted = false;
    
    if (packagePath.endsWith(".deb")) {
        // Extract .deb
        QString arDir = tempExtract + "/ar";
        QDir().mkpath(arDir);
        
        ProcessResult arResult = SubprocessWrapper::execute("ar", {"x", packagePath}, arDir, 30000);
        if (arResult.success) {
            QDir arDirObj(arDir);
            QStringList dataFiles = arDirObj.entryList({"data.tar.*"}, QDir::Files);
            if (!dataFiles.isEmpty()) {
                QString dataTar = arDirObj.absoluteFilePath(dataFiles.first());
                QString dataDir = tempExtract + "/data";
                QDir().mkpath(dataDir);
                
                QStringList tarArgs;
                if (dataTar.endsWith(".gz")) {
                    tarArgs = {"-xzf", dataTar, "-C", dataDir};
                } else if (dataTar.endsWith(".xz")) {
                    tarArgs = {"-xJf", dataTar, "-C", dataDir};
                } else {
                    tarArgs = {"-xf", dataTar, "-C", dataDir};
                }
                
                ProcessResult tarResult = SubprocessWrapper::execute("tar", tarArgs, {}, 60000);
                extracted = tarResult.success;
            }
        }
    } else if (packagePath.endsWith(".rpm")) {
        // Extract .rpm
        QString dataDir = tempExtract + "/data";
        QDir().mkpath(dataDir);
        
        // Use rpm2cpio and cpio
        QString command = QString("rpm2cpio \"%1\" | cpio -idm --quiet").arg(packagePath);
        ProcessResult result = SubprocessWrapper::execute("/bin/sh", {"-c", command}, dataDir, 60000);
        extracted = result.success;
    } else if (packagePath.contains(".pkg.tar")) {
        // Extract Arch Linux .pkg.tar.* (zst, xz, gz, etc.)
        QString dataDir = tempExtract + "/data";
        QDir().mkpath(dataDir);
        
        QStringList tarArgs;
        if (packagePath.endsWith(".zst")) {
            tarArgs = {"--zstd", "-xf", packagePath, "-C", dataDir};
        } else if (packagePath.endsWith(".xz")) {
            tarArgs = {"-xJf", packagePath, "-C", dataDir};
        } else if (packagePath.endsWith(".gz")) {
            tarArgs = {"-xzf", packagePath, "-C", dataDir};
        } else {
            tarArgs = {"-xf", packagePath, "-C", dataDir};
        }
        
        ProcessResult result = SubprocessWrapper::execute("tar", tarArgs, {}, 60000);
        extracted = result.success;
    }
    
    if (!extracted) {
        SubprocessWrapper::removeDirectory(tempExtract);
        return libs;
    }
    
    // Find and copy .so files, preserving directory structure
    QString dataDir = tempExtract + "/data";
    QDir dataQDir(dataDir);
    
    // Create lib directory in output
    QString libDir = outputDir + "/usr/lib";
    QDir().mkpath(libDir);
    
    // Recursively find and copy .so files, preserving structure
    std::function<void(const QString&, const QString&)> findLibs = [&](const QString& srcDir, const QString& basePath) {
        QDir d(srcDir);
        for (const QFileInfo& info : d.entryInfoList(QDir::Files)) {
            QString name = info.fileName();
            if (name.contains(".so")) {
                // Calculate relative path from usr/lib
                QString relPath = info.absoluteFilePath();
                if (relPath.contains("/usr/lib/")) {
                    relPath = relPath.mid(relPath.indexOf("/usr/lib/") + 9); // Skip "/usr/lib/"
                } else {
                    relPath = name; // Just filename if not in usr/lib structure
                }
                
                QString dest = libDir + "/" + relPath;
                QFileInfo destInfo(dest);
                QDir().mkpath(destInfo.absolutePath());
                
                if (QFile::copy(info.absoluteFilePath(), dest)) {
                    libs.append(dest);
                    emit log(QString("  Copied library: %1 -> %2").arg(name).arg(relPath));
                    
                    // Create symlinks for .so files (e.g., libgsl.so.28 -> libgsl.so.28.0.0)
                    if (name.contains(".so.") && !name.endsWith(".so")) {
                        // Extract base name (e.g., libgsl.so.28 from libgsl.so.28.0.0)
                        QStringList parts = name.split('.');
                        if (parts.size() >= 3) {
                            QString symlinkName = parts[0] + "." + parts[1] + "." + parts[2]; // libgsl.so.28
                            QString symlinkPath = libDir + "/" + symlinkName;
                            if (!QFile::exists(symlinkPath)) {
                                // Remove existing file if it's not a symlink
                                if (QFile::exists(symlinkPath)) {
                                    QFile::remove(symlinkPath);
                                }
                                // Create symlink with relative path
                                QFile::link(name, symlinkPath);
                                emit log(QString("  Created symlink: %1 -> %2").arg(symlinkName).arg(name));
                            }
                        }
                        // Also create libgsl.so -> libgsl.so.28 if needed
                        if (parts.size() >= 3) {
                            QString baseName = parts[0] + "." + parts[1]; // libgsl.so
                            QString basePath = libDir + "/" + baseName;
                            if (!QFile::exists(basePath)) {
                                QString targetName = parts[0] + "." + parts[1] + "." + parts[2]; // libgsl.so.28
                                QFile::link(targetName, basePath);
                                emit log(QString("  Created symlink: %1 -> %2").arg(baseName).arg(targetName));
                            }
                        }
                    }
                }
            }
        }
        for (const QFileInfo& subdir : d.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            findLibs(subdir.absoluteFilePath(), basePath);
        }
    };
    
    findLibs(dataDir, dataDir);
    
    // Cleanup
    SubprocessWrapper::removeDirectory(tempExtract);
    
    return libs;
}

QString DependencyResolver::findPackageForLibrary(const QString& libName) {
    PackageManager pm = RepositoryBrowser::detectPackageManager();
    ProcessResult result;
    
    // Hardcoded mappings for common KDE5 libraries (pacman -F requires synced database)
    static QMap<QString, QString> kf5Mappings = {
        {"libKF5Crash.so", "kcrash5"},
        {"libKF5I18n.so", "ki18n5"},
        {"libKF5ConfigCore.so", "kconfig5"},
        {"libKF5ConfigGui.so", "kconfig5"},
        {"libKF5CoreAddons.so", "kcoreaddons5"},
        {"libKF5GuiAddons.so", "kguiaddons5"},
        {"libKF5WidgetsAddons.so", "kwidgetsaddons5"},
        {"libKF5Completion.so", "kcompletion5"},
        {"libKF5ItemViews.so", "kitemviews5"},
        {"libKF5WindowSystem.so", "kwindowsystem5"},
        {"libKF5Archive.so", "karchive5"},
        {"libKF5Codecs.so", "kcodecs5"},
        {"libKF5Auth.so", "kauth5"},
        {"libKF5Service.so", "kservice5"},
        {"libKF5TextWidgets.so", "ktextwidgets5"},
        {"libKF5XmlGui.so", "kxmlgui5"},
        {"libKF5ItemModels.so", "kitemmodels5"},
        {"libKF5Notifications.so", "knotifications5"},
        {"libKF5JobWidgets.so", "kjobwidgets5"},
        {"libKF5KIO.so", "kio5"},
        {"libKF5Bookmarks.so", "kbookmarks5"},
        {"libgsl.so", "gsl"},
        {"libmlt-7.so", "mlt"},
        {"libmlt++-7.so", "mlt"},
        {"libquazip1-qt5.so", "quazip-qt5"},
        {"libgsl.so", "gsl"},
    };
    
    // Try to match KF5 library by prefix
    for (auto it = kf5Mappings.begin(); it != kf5Mappings.end(); ++it) {
        if (libName.startsWith(it.key())) {
            emit log(QString("  Using hardcoded mapping: %1 -> %2").arg(libName).arg(it.value()));
            return it.value();
        }
    }
    
    switch (pm) {
        case PackageManager::PACMAN: {
            // Use pacman -F to find which package provides the library
            result = SubprocessWrapper::execute("pacman", {"-F", libName}, {}, 30000);
            if (result.success && !result.stdoutOutput.isEmpty()) {
                // Parse output: "extra/kcrash 5.xxx.x [installed]\n    /usr/lib/libKF5Crash.so.5"
                QStringList lines = result.stdoutOutput.split('\n');
                for (const QString& line : lines) {
                    QString trimmed = line.trimmed();
                    if (!trimmed.isEmpty() && !trimmed.startsWith("/") && trimmed.contains('/')) {
                        // Format: repo/package version
                        QString pkgName = trimmed.split(' ').first();
                        if (pkgName.contains('/')) {
                            pkgName = pkgName.split('/').last();
                        }
                        emit log(QString("  Found package for %1: %2").arg(libName).arg(pkgName));
                        return pkgName;
                    }
                }
            }
            break;
        }
        case PackageManager::APT: {
            // Use apt-file search
            result = SubprocessWrapper::execute("apt-file", {"search", libName}, {}, 30000);
            if (result.success && !result.stdoutOutput.isEmpty()) {
                // Parse: package: /path/to/lib
                QStringList lines = result.stdoutOutput.split('\n');
                if (!lines.isEmpty()) {
                    QString firstLine = lines.first().trimmed();
                    if (firstLine.contains(':')) {
                        QString pkgName = firstLine.split(':').first().trimmed();
                        emit log(QString("  Found package for %1: %2").arg(libName).arg(pkgName));
                        return pkgName;
                    }
                }
            }
            break;
        }
        case PackageManager::DNF: {
            // Use dnf provides
            result = SubprocessWrapper::execute("dnf", {"provides", "*/" + libName}, {}, 30000);
            if (result.success && !result.stdoutOutput.isEmpty()) {
                QStringList lines = result.stdoutOutput.split('\n');
                for (const QString& line : lines) {
                    if (line.contains("-") && !line.startsWith(" ")) {
                        QString pkgName = line.split('-').first().trimmed();
                        if (!pkgName.isEmpty()) {
                            emit log(QString("  Found package for %1: %2").arg(libName).arg(pkgName));
                            return pkgName;
                        }
                    }
                }
            }
            break;
        }
        default:
            break;
    }
    
    return QString();
}

QStringList DependencyResolver::findMissingLibraries(const QString& binaryPath) {
    QStringList missing;
    
    // Run ldd on the binary
    ProcessResult result = SubprocessWrapper::execute("ldd", {binaryPath}, {}, 30000);
    if (!result.success) {
        emit log(QString("  Failed to run ldd on %1").arg(binaryPath));
        return missing;
    }
    
    // Parse ldd output for "not found" lines
    QStringList lines = result.stdoutOutput.split('\n');
    for (const QString& line : lines) {
        if (line.contains("not found")) {
            // Format: "libXXX.so.N => not found"
            QString trimmed = line.trimmed();
            int arrowPos = trimmed.indexOf("=>");
            if (arrowPos > 0) {
                QString libName = trimmed.left(arrowPos).trimmed();
                if (!libName.isEmpty() && !missing.contains(libName)) {
                    missing.append(libName);
                }
            }
        }
    }
    
    return missing;
}

bool DependencyResolver::resolveMissingLibraries(const QString& binaryPath, const QString& appDir) {
    emit log(QString("=== Analyzing missing libraries for %1 ===").arg(QFileInfo(binaryPath).fileName()));
    
    // Set LD_LIBRARY_PATH to include AppDir libs for proper analysis
    QString libPath = appDir + "/usr/lib";
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    QString existingLdPath = env.value("LD_LIBRARY_PATH");
    env.insert("LD_LIBRARY_PATH", libPath + ":" + existingLdPath);
    
    // Run ldd with AppDir libs
    ProcessResult result = SubprocessWrapper::execute("ldd", {binaryPath}, {}, 30000, env);
    if (!result.success) {
        emit log(QString("  Failed to analyze binary"));
        return false;
    }
    
    // Parse missing libraries
    QStringList missing;
    QStringList lines = result.stdoutOutput.split('\n');
    for (const QString& line : lines) {
        if (line.contains("not found")) {
            QString trimmed = line.trimmed();
            int arrowPos = trimmed.indexOf("=>");
            if (arrowPos > 0) {
                QString libName = trimmed.left(arrowPos).trimmed();
                if (!libName.isEmpty() && !missing.contains(libName)) {
                    missing.append(libName);
                }
            }
        }
    }
    
    if (missing.isEmpty()) {
        emit log("  No missing libraries found");
        return true;
    }
    
    emit log(QString("  Found %1 missing libraries").arg(missing.size()));
    
    // Deduplicate by finding packages
    QSet<QString> packagesToDownload;
    for (const QString& lib : missing) {
        if (shouldExclude(lib)) {
            emit log(QString("  Skipping system library: %1").arg(lib));
            continue;
        }
        
        QString pkgName = findPackageForLibrary(lib);
        if (!pkgName.isEmpty()) {
            packagesToDownload.insert(pkgName);
        } else {
            emit log(QString("  WARNING: No package found for %1").arg(lib));
        }
    }
    
    emit log(QString("  Need to download %1 packages").arg(packagesToDownload.size()));
    
    // Download and extract each package
    int downloaded = 0;
    for (const QString& pkg : packagesToDownload) {
        emit log(QString("  Downloading package: %1").arg(pkg));
        if (downloadAndExtract(pkg, appDir)) {
            downloaded++;
        }
    }
    
    emit log(QString("=== Downloaded %1/%2 packages ===").arg(downloaded).arg(packagesToDownload.size()));
    
    // Verify libraries are now available
    if (downloaded > 0) {
        emit log("=== Verifying downloaded libraries ===");
        ProcessResult verifyResult = SubprocessWrapper::execute("ldd", {binaryPath}, {}, 30000, env);
        if (verifyResult.success) {
            QStringList verifyLines = verifyResult.stdoutOutput.split('\n');
            int stillMissing = 0;
            for (const QString& line : verifyLines) {
                if (line.contains("not found")) {
                    stillMissing++;
                    QString trimmed = line.trimmed();
                    int arrowPos = trimmed.indexOf("=>");
                    if (arrowPos > 0) {
                        QString libName = trimmed.left(arrowPos).trimmed();
                        emit log(QString("  Still missing: %1").arg(libName));
                    }
                }
            }
            if (stillMissing == 0) {
                emit log("  All libraries resolved successfully!");
            } else {
                emit log(QString("  WARNING: %1 libraries still missing").arg(stillMissing));
            }
        }
    }
    
    return downloaded > 0;
}

