#include "packagetoappimagepipeline.h"
#include "utils.h"
#include <QStandardPaths>
#include <QDir>
#include <QDirIterator>
#include <QDebug>
#include <QDateTime>
#include <QFileInfo>
#include <QMetaObject>
#include <QRegularExpression>

PackageToAppImagePipeline::PackageToAppImagePipeline(QObject* parent)
    : QObject(parent)
    , m_packageType(PackageFormat::Unknown)
    , m_cancelled(false)
    , m_debParser(new DebParser())
    , m_rpmParser(new RpmParser())
    , m_tarballParser(new TarballParser())
    , m_analyzer(new DependencyAnalyzer())
    , m_appDirBuilder(new AppDirBuilder())
    , m_appImageBuilder(new AppImageBuilder(this))
    , m_sizeOptimizer(new SizeOptimizer(this))
    , m_dependencyResolver(new DependencyResolver(this))
{
    // Forward log signals from AppImageBuilder
    connect(m_appImageBuilder, &AppImageBuilder::log, this, &PackageToAppImagePipeline::log);
    
    // Forward log signals from SizeOptimizer
    connect(m_sizeOptimizer, &SizeOptimizer::log, this, &PackageToAppImagePipeline::log);
    
    // Forward log signals from DependencyResolver
    connect(m_dependencyResolver, &DependencyResolver::log, this, &PackageToAppImagePipeline::log);
}

PackageToAppImagePipeline::~PackageToAppImagePipeline() {
    cleanup();
    delete m_debParser;
    delete m_rpmParser;
    delete m_tarballParser;
    delete m_analyzer;
    delete m_appDirBuilder;
    delete m_appImageBuilder;
    // m_sizeOptimizer is deleted by QObject parent
}

void PackageToAppImagePipeline::setPackagePath(const QString& packagePath) {
    m_packagePath = packagePath;
    m_packageType = detectPackageType(packagePath);
}

void PackageToAppImagePipeline::setOutputPath(const QString& outputPath) {
    m_outputPath = outputPath;
}

void PackageToAppImagePipeline::setOptimizationSettings(const OptimizationSettings& settings) {
    m_optimizationSettings = settings;
}

OptimizationSettings PackageToAppImagePipeline::optimizationSettings() const {
    return m_optimizationSettings;
}

void PackageToAppImagePipeline::setDependencySettings(const DependencySettings& settings) {
    m_dependencySettings = settings;
}

DependencySettings PackageToAppImagePipeline::dependencySettings() const {
    return m_dependencySettings;
}

void PackageToAppImagePipeline::setSudoPassword(const QString& password) {
    m_dependencyResolver->setSudoPassword(password);
}

void PackageToAppImagePipeline::start() {
    m_cancelled = false;
    QMetaObject::invokeMethod(this, "process", Qt::QueuedConnection);
}

void PackageToAppImagePipeline::cancel() {
    m_cancelled = true;
    emit log("Operation cancelled by user");
}

PackageFormat PackageToAppImagePipeline::detectPackageType(const QString& packagePath) {
    QFileInfo info(packagePath);
    QString suffix = info.suffix().toLower();
    
    if (suffix == "deb") {
        return PackageFormat::Deb;
    } else if (suffix == "rpm") {
        return PackageFormat::Rpm;
    } else if (TarballParser::isSupportedTarball(packagePath)) {
        return PackageFormat::Tarball;
    }
    
    return PackageFormat::Unknown;
}

void PackageToAppImagePipeline::process() {
    emit progress(0, "Starting conversion...");
    QString packageTypeStr;
    if (m_packageType == PackageFormat::Deb) {
        packageTypeStr = ".deb";
    } else if (m_packageType == PackageFormat::Rpm) {
        packageTypeStr = ".rpm";
    } else if (m_packageType == PackageFormat::Tarball) {
        packageTypeStr = "tarball";
    } else {
        packageTypeStr = "package";
    }
    emit log(QString("=== Starting %1 to AppImage conversion ===").arg(packageTypeStr));
    
    // Create temp directory in /tmp (RAM-optimized)
    m_tempDir = QString("/tmp/appalchemist-%1")
        .arg(QString::number(QDateTime::currentMSecsSinceEpoch()));
    
    if (!SubprocessWrapper::createDirectory(m_tempDir)) {
        emit error("Failed to create temporary directory");
        emit finished();
        return;
    }
    
    emit log(QString("Using temporary directory: %1").arg(m_tempDir));
    
    // Step 1: Validate input
    if (m_cancelled) {
        emit finished();
        return;
    }
    
    emit progress(10, QString("Validating %1 file...").arg(packageTypeStr));
    if (!validateInput()) {
        emit error(QString("Invalid %1 file").arg(packageTypeStr));
        cleanup();
        emit finished();
        return;
    }
    
    // Step 2: Extract .deb
    if (m_cancelled) {
        emit finished();
        return;
    }
    
    emit progress(20, QString("Extracting %1 package...").arg(packageTypeStr));
    if (!extractPackage()) {
        emit error(QString("Failed to extract %1 package or no executables found").arg(packageTypeStr));
        cleanup();
        emit finished();
        return;
    }
    
    // Step 3: Analyze dependencies
    if (m_cancelled) {
        emit finished();
        return;
    }
    
    emit progress(40, "Analyzing dependencies...");
    if (!analyzeDependencies()) {
        emit error("Failed to analyze dependencies");
        cleanup();
        emit finished();
        return;
    }

    if (!executeConversionPlan()) {
        QString errorMsg = "Failed to build AppImage.\n\n";
        if (m_appImageBuilder->findAppImageTool().isEmpty()) {
            errorMsg += "AppImageTool not found!\n\n";
            errorMsg += "Please download appimagetool from:\n";
            errorMsg += "https://github.com/AppImage/AppImageKit/releases\n\n";
            errorMsg += "And place it in:\n";
            errorMsg += "  - thirdparty/appimagetool (in project directory)\n";
            errorMsg += "  - /usr/bin/appimagetool (system-wide)\n";
            errorMsg += "  - /usr/local/bin/appimagetool (local)";
        } else {
            errorMsg += "Check the logs for details.";
        }
        emit error(errorMsg);
        cleanup();
        emit finished();
        return;
    }
    
    // Success
    emit progress(100, "Conversion completed successfully!");
    emit log("=== Conversion completed successfully ===");
    emit success(m_outputPath);
    
    cleanup();
    emit finished();
}

bool PackageToAppImagePipeline::executeConversionPlan() {
    logConversionPlan();

    if (shouldUseFastPath()) {
        emit progress(55, "Preparing fast conversion path...");
        if (executeFastPath()) {
            return true;
        }

        if (!m_conversionPlan.allowFallback) {
            emit log("Fast path failed and fallback is disabled.");
            return false;
        }

        emit log("Fast path is not implemented for this profile yet. Falling back to legacy conversion path.");
    }

    if (shouldUseRepairPath()) {
        emit progress(55, "Preparing repair-oriented conversion path...");
        if (executeRepairPath()) {
            return true;
        }

        if (!m_conversionPlan.allowFallback) {
            emit log("Repair path failed and fallback is disabled.");
            return false;
        }

        emit log("Repair path did not fully stabilize the package. Falling back to legacy conversion path.");
    }

    return executeFallbackPath();
}

bool PackageToAppImagePipeline::executeFastPath() {
    if (m_packageProfile.applicationProfile != ApplicationProfile::NativeDesktop &&
        m_packageProfile.applicationProfile != ApplicationProfile::NativeCli) {
        emit log("Fast path currently supports only native desktop and CLI profiles.");
        return false;
    }

    if (m_cancelled) {
        return false;
    }

    emit log("Attempting fast conversion path for native application profile.");
    emit progress(55, "Building AppDir structure...");
    if (!buildAppDir()) {
        emit log("Fast path failed while building AppDir.");
        return false;
    }

    const QString mainExec = findPrimaryAppDirExecutable();
    if (mainExec.isEmpty()) {
        emit log("Fast path could not locate the primary executable inside AppDir.");
        return false;
    }

    if (!verifyAppDirReadiness(mainExec, m_packageProfile.isGraphical)) {
        return false;
    }

    if (m_cancelled) {
        return false;
    }

    if (!optimizeBuiltAppDir("Fast path")) {
        return false;
    }

    if (m_cancelled) {
        return false;
    }

    if (!probeAppDirRuntime("Fast path", m_packageProfile.applicationProfile == ApplicationProfile::NativeCli)) {
        return false;
    }

    if (m_cancelled) {
        return false;
    }

    if (!packageBuiltAppDir("Fast path")) {
        return false;
    }

    emit log("Fast path completed successfully.");
    return true;
}

bool PackageToAppImagePipeline::executeRepairPath() {
    emit log(QString("Attempting repair-oriented path for profile '%1'.")
             .arg(PackageClassifier::applicationProfileToString(m_packageProfile.applicationProfile)));

    if (m_cancelled) {
        return false;
    }

    emit progress(55, "Building AppDir structure...");
    if (!buildAppDir()) {
        emit log("Repair path failed while building AppDir.");
        return false;
    }

    const QString mainExec = findPrimaryAppDirExecutable();
    if (mainExec.isEmpty()) {
        emit log("Repair path could not locate the primary executable inside AppDir.");
        return false;
    }

    if (!m_dependencySettings.enabled) {
        emit log("Repair path requires dependency resolution to be enabled. Deferring to legacy fallback.");
        return false;
    }

    if (!resolveAppDirDependencies(mainExec, "Repair path", true)) {
        return false;
    }

    if (m_cancelled) {
        return false;
    }

    if (!verifyAppDirReadiness(mainExec, m_packageProfile.isGraphical)) {
        return false;
    }

    if (!optimizeBuiltAppDir("Repair path")) {
        return false;
    }

    if (m_cancelled) {
        return false;
    }

    if (!probeAppDirRuntime("Repair path", false)) {
        return false;
    }

    if (m_cancelled) {
        return false;
    }

    if (!packageBuiltAppDir("Repair path")) {
        return false;
    }

    emit log("Repair-oriented path completed successfully.");
    return true;
}

bool PackageToAppImagePipeline::executeFallbackPath() {
    if (m_cancelled) {
        return false;
    }

    emit progress(55, "Building AppDir structure...");
    if (!buildAppDir()) {
        emit log("Fallback path failed while building AppDir");
        return false;
    }

    if (m_cancelled) {
        return false;
    }

    if (m_dependencySettings.enabled) {
        QString mainExec;
        if (!m_metadata.executables.isEmpty()) {
            mainExec = m_appDirPath + "/usr/bin/" + QFileInfo(m_metadata.executables.first()).fileName();
            if (!QFileInfo::exists(mainExec)) {
                QDir binDir(m_appDirPath + "/usr/bin");
                QStringList execs = binDir.entryList(QDir::Files | QDir::Executable);
                if (!execs.isEmpty()) {
                    mainExec = binDir.absoluteFilePath(execs.first());
                }
            }
        }

        if (mainExec.isEmpty() || !QFileInfo::exists(mainExec)) {
            emit log("WARNING: Could not find main executable for dependency analysis");
        } else if (!resolveAppDirDependencies(mainExec, "Fallback path", false)) {
            emit log("WARNING: Fallback dependency repair did not fully resolve runtime issues");
        }
    }

    if (m_cancelled) {
        return false;
    }

    if (!optimizeBuiltAppDir("Fallback path")) {
        return false;
    }

    if (m_cancelled) {
        return false;
    }

    if (!probeAppDirRuntime("Fallback path", false)) {
        emit log("WARNING: Fallback runtime probe reported issues, continuing to package anyway");
    }

    if (m_cancelled) {
        return false;
    }

    if (!packageBuiltAppDir("Fallback path")) {
        return false;
    }

    return true;
}

bool PackageToAppImagePipeline::shouldUseFastPath() const {
    return m_conversionPlan.initialMode == ConversionMode::FastPathPreferred;
}

bool PackageToAppImagePipeline::shouldUseRepairPath() const {
    return m_conversionPlan.initialMode == ConversionMode::RepairFallback;
}

void PackageToAppImagePipeline::logConversionPlan() {
    emit log(QString("Conversion plan: %1").arg(m_conversionPlan.summary()));
}

bool PackageToAppImagePipeline::verifyAppDirReadiness(const QString& executablePath,
                                                      bool requireDesktopEntry) const {
    const QFileInfo appRunInfo(QDir(m_appDirPath).absoluteFilePath("AppRun"));
    if (!appRunInfo.exists() || !appRunInfo.isFile()) {
        emit const_cast<PackageToAppImagePipeline*>(this)->log("AppDir verification failed: AppRun is missing.");
        return false;
    }

    if (requireDesktopEntry) {
        const QDir appDir(m_appDirPath);
        const QString rootDesktop = appDir.absoluteFilePath(QString("%1.desktop").arg(m_metadata.package));
        const QDir desktopDir(appDir.absoluteFilePath("usr/share/applications"));
        const bool hasRootDesktop = QFileInfo::exists(rootDesktop);
        const bool hasAnyDesktop = desktopDir.exists() && !desktopDir.entryList({"*.desktop"}, QDir::Files).isEmpty();
        if (!hasRootDesktop && !hasAnyDesktop) {
            emit const_cast<PackageToAppImagePipeline*>(this)->log(
                "AppDir verification failed: desktop entry is missing for a graphical package.");
            return false;
        }
    }

    if (executablePath.isEmpty() || !QFileInfo::exists(executablePath)) {
        emit const_cast<PackageToAppImagePipeline*>(this)->log(
            "AppDir verification failed: primary executable is missing after build.");
        return false;
    }

    emit const_cast<PackageToAppImagePipeline*>(this)->log("AppDir verification passed.");
    return true;
}

bool PackageToAppImagePipeline::resolveAppDirDependencies(const QString& executablePath,
                                                          const QString& stageLabel,
                                                          bool requiredForSuccess) {
    emit progress(65, "Resolving missing libraries...");
    m_dependencyResolver->setSettings(m_dependencySettings);
    emit log(QString("%1 analyzing dependencies for: %2").arg(stageLabel, executablePath));

    const bool resolved = m_dependencyResolver->resolveMissingLibraries(executablePath, m_appDirPath);
    const QStringList remainingMissing = findMissingRuntimeLibraries(executablePath);

    if (remainingMissing.isEmpty()) {
        emit log(QString("%1 dependency verification passed.").arg(stageLabel));
        return true;
    }

    emit log(QString("%1 still sees unresolved runtime libraries: %2")
             .arg(stageLabel, remainingMissing.join(", ")));

    if (requiredForSuccess) {
        return false;
    }

    return resolved;
}

bool PackageToAppImagePipeline::optimizeBuiltAppDir(const QString& stageLabel) {
    if (!m_optimizationSettings.enabled) {
        return true;
    }

    emit progress(75, "Optimizing AppDir size...");
    m_sizeOptimizer->setSettings(m_optimizationSettings);
    if (!m_sizeOptimizer->optimizeAppDir(m_appDirPath)) {
        emit log(QString("WARNING: %1 size optimization failed, continuing anyway...").arg(stageLabel));
    }

    return true;
}

bool PackageToAppImagePipeline::packageBuiltAppDir(const QString& stageLabel) {
    emit progress(85, "Building AppImage...");
    if (!buildAppImage()) {
        emit log(QString("%1 failed while building AppImage.").arg(stageLabel));
        return false;
    }

    return true;
}

bool PackageToAppImagePipeline::probeAppDirRuntime(const QString& stageLabel, bool requiredForSuccess) {
    const QString appRunPath = QDir(m_appDirPath).absoluteFilePath("AppRun");
    if (!QFileInfo::exists(appRunPath)) {
        emit log(QString("%1 runtime probe skipped: AppRun is missing.").arg(stageLabel));
        return !requiredForSuccess;
    }

    if (!probeAppRunSyntax(appRunPath)) {
        emit log(QString("%1 runtime probe failed: AppRun syntax check failed.").arg(stageLabel));
        return !requiredForSuccess ? false : false;
    }

    if (m_packageProfile.applicationProfile != ApplicationProfile::NativeCli &&
        m_packageProfile.applicationProfile != ApplicationProfile::Script) {
        emit log(QString("%1 runtime probe: syntax check passed; live probe skipped for non-CLI profile.")
                 .arg(stageLabel));
        return true;
    }

    emit progress(80, "Running AppDir probe...");
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("APPDIR", m_appDirPath);
    env.insert("HERE", m_appDirPath);
    env.insert("LD_LIBRARY_PATH", QString("%1/usr/lib:%2").arg(m_appDirPath, env.value("LD_LIBRARY_PATH")));
    env.insert("PATH", QString("%1/usr/bin:%2/usr/sbin:%3/usr/games:%4")
               .arg(m_appDirPath, m_appDirPath, m_appDirPath, env.value("PATH")));
    env.insert("NO_AT_BRIDGE", "1");

    const ProcessResult probeResult = SubprocessWrapper::execute(
        appRunPath,
        {"--help"},
        m_appDirPath,
        2000,
        env);

    if (probeResult.success) {
        emit log(QString("%1 runtime probe passed.").arg(stageLabel));
        return true;
    }

    const QString combinedOutput = probeResult.stderrOutput + "\n" + probeResult.stdoutOutput;
    const bool looksHealthy = combinedOutput.contains("Usage", Qt::CaseInsensitive)
        || combinedOutput.contains("help", Qt::CaseInsensitive)
        || combinedOutput.contains("version", Qt::CaseInsensitive);

    if (looksHealthy) {
        emit log(QString("%1 runtime probe produced non-zero exit but returned recognizable help output.")
                 .arg(stageLabel));
        return true;
    }

    emit log(QString("%1 runtime probe failed: %2").arg(
        stageLabel,
        probeResult.errorMessage.isEmpty() ? QString("unknown error") : probeResult.errorMessage));

    return !requiredForSuccess;
}

bool PackageToAppImagePipeline::probeAppRunSyntax(const QString& appRunPath) const {
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
        const ProcessResult syntaxResult = SubprocessWrapper::execute(
            "/bin/sh",
            {"-n", appRunPath},
            {},
            5000);
        return syntaxResult.success;
    }

    return true;
}

QString PackageToAppImagePipeline::findPrimaryAppDirExecutable() const {
    const QString appDirRoot = m_appDirPath;
    if (appDirRoot.isEmpty()) {
        return QString();
    }

    auto mapToAppDirPath = [appDirRoot](const QString& sourcePath) -> QString {
        if (sourcePath.isEmpty()) {
            return QString();
        }

        QString relativePath = sourcePath;
        if (relativePath.contains("/data/")) {
            relativePath = relativePath.section("/data/", 1);
        } else if (relativePath.startsWith('/')) {
            relativePath = relativePath.mid(1);
        }

        if (relativePath.startsWith("usr/bin/") ||
            relativePath.startsWith("usr/sbin/") ||
            relativePath.startsWith("usr/games/") ||
            relativePath.startsWith("usr/lib/") ||
            relativePath.startsWith("opt/")) {
            return QDir(appDirRoot).absoluteFilePath(relativePath);
        }

        const QFileInfo sourceInfo(sourcePath);
        if (sourceInfo.suffix().compare("jar", Qt::CaseInsensitive) == 0) {
            return QDir(appDirRoot).absoluteFilePath(QString("usr/games/%1").arg(sourceInfo.fileName()));
        }

        return QDir(appDirRoot).absoluteFilePath(QString("usr/bin/%1").arg(sourceInfo.fileName()));
    };

    QStringList candidates;
    if (!m_metadata.mainExecutable.isEmpty()) {
        candidates << mapToAppDirPath(m_metadata.mainExecutable);
    }

    for (const QString& exec : m_metadata.executables) {
        const QString mappedPath = mapToAppDirPath(exec);
        if (!mappedPath.isEmpty() && !candidates.contains(mappedPath)) {
            candidates << mappedPath;
        }
    }

    for (const QString& candidate : candidates) {
        const QFileInfo info(candidate);
        if (info.exists() && info.isFile()) {
            return candidate;
        }
    }

    const QStringList fallbackDirs = {
        QDir(appDirRoot).absoluteFilePath("usr/bin"),
        QDir(appDirRoot).absoluteFilePath("usr/games"),
        QDir(appDirRoot).absoluteFilePath("usr/sbin")
    };

    for (const QString& dirPath : fallbackDirs) {
        const QDir dir(dirPath);
        if (!dir.exists()) {
            continue;
        }

        const QStringList executables = dir.entryList(QDir::Files | QDir::Executable, QDir::Name);
        if (!executables.isEmpty()) {
            return dir.absoluteFilePath(executables.first());
        }
    }

    return QString();
}

QStringList PackageToAppImagePipeline::findMissingRuntimeLibraries(const QString& executablePath) const {
    QStringList missingLibraries;
    if (executablePath.isEmpty() || !QFileInfo::exists(executablePath)) {
        return missingLibraries;
    }

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QString existingLdPath = env.value("LD_LIBRARY_PATH");
    env.insert("LD_LIBRARY_PATH", QString("%1/usr/lib:%2").arg(m_appDirPath, existingLdPath));

    const ProcessResult result = SubprocessWrapper::execute("ldd", {executablePath}, {}, 15000, env);
    if (!result.success) {
        missingLibraries << "__ldd_failed__";
        return missingLibraries;
    }

    const QStringList lines = result.stdoutOutput.split('\n', Qt::SkipEmptyParts);
    for (const QString& line : lines) {
        if (!line.contains("not found")) {
            continue;
        }

        const QString trimmed = line.trimmed();
        const int arrowPos = trimmed.indexOf("=>");
        if (arrowPos > 0) {
            const QString libName = trimmed.left(arrowPos).trimmed();
            if (!libName.isEmpty() && !missingLibraries.contains(libName)) {
                missingLibraries << libName;
            }
        }
    }

    return missingLibraries;
}

bool PackageToAppImagePipeline::validateInput() {
    if (m_packagePath.isEmpty() || !QFileInfo::exists(m_packagePath)) {
        emit log("ERROR: Package file not found");
        return false;
    }
    
    if (m_packageType == PackageFormat::Deb) {
        if (!m_debParser->validateDebFile(m_packagePath)) {
            emit log("ERROR: Invalid .deb file format");
            return false;
        }
        emit log(QString("Valid .deb file: %1").arg(m_packagePath));
    } else if (m_packageType == PackageFormat::Rpm) {
        if (!m_rpmParser->validateRpmFile(m_packagePath)) {
            emit log("ERROR: Invalid .rpm file format");
            return false;
        }
        emit log(QString("Valid .rpm file: %1").arg(m_packagePath));
    } else if (m_packageType == PackageFormat::Tarball) {
        if (!m_tarballParser->validateTarball(m_packagePath)) {
            emit log("ERROR: Invalid tarball file format");
            return false;
        }
        emit log(QString("Valid tarball file: %1").arg(m_packagePath));
    } else {
        emit log("ERROR: Unknown package type");
        return false;
    }
    
    return true;
}

bool PackageToAppImagePipeline::extractPackage() {
    m_extractedPackageDir = QString("%1/extracted").arg(m_tempDir);
    
    bool success = false;
    if (m_packageType == PackageFormat::Deb) {
        if (!m_debParser->extractDeb(m_packagePath, m_extractedPackageDir)) {
            emit log("ERROR: Failed to extract .deb package");
            return false;
        }
        emit log("Successfully extracted .deb package");
        m_metadata = m_debParser->parseMetadata(m_extractedPackageDir);
    } else if (m_packageType == PackageFormat::Tarball) {
        if (!m_tarballParser->extractTarball(m_packagePath, m_extractedPackageDir)) {
            emit log("ERROR: Failed to extract tarball");
            return false;
        }
        emit log("Successfully extracted tarball");
        m_metadata = m_tarballParser->parseMetadata(m_extractedPackageDir);
        
        // Use tarball filename for package name if not found
        if (m_metadata.package.isEmpty()) {
            QFileInfo info(m_packagePath);
            QString baseName = info.baseName();
            // Remove common extensions from basename
            baseName.remove(QRegularExpression(R"(\.(tar|linux|x86_64|amd64)$)", QRegularExpression::CaseInsensitiveOption));
            m_metadata.package = baseName;
        }
        
        emit log(QString("Detected package: %1, Version: %2").arg(m_metadata.package).arg(m_metadata.version));
    } else if (m_packageType == PackageFormat::Rpm) {
        if (!m_rpmParser->extractRpm(m_packagePath, m_extractedPackageDir)) {
            emit log("ERROR: Failed to extract .rpm package");
            return false;
        }
        emit log("Successfully extracted .rpm package");
        
        // Try to get metadata from RPM file directly using rpm command (if available)
        ProcessResult rpmInfo = SubprocessWrapper::execute("rpm", {
            "-qp", "--queryformat", "%{NAME}\n%{VERSION}\n%{SUMMARY}\n", m_packagePath
        });
        
        if (rpmInfo.success && !rpmInfo.stdoutOutput.isEmpty()) {
            QStringList lines = rpmInfo.stdoutOutput.trimmed().split('\n');
            if (lines.size() >= 2) {
                m_metadata.package = lines[0].trimmed();
                m_metadata.version = lines[1].trimmed();
                if (lines.size() >= 3) {
                    m_metadata.description = lines[2].trimmed();
                }
                emit log(QString("Got metadata from RPM: %1 %2").arg(m_metadata.package).arg(m_metadata.version));
            }
        } else {
            // Fallback: try to extract metadata from filename
            QFileInfo packageInfo(m_packagePath);
            QString baseName = packageInfo.baseName();
            emit log(QString("rpm command not available, trying to extract metadata from filename: %1").arg(baseName));
            
            // Try to parse filename like: package-version-release.arch.rpm
            // Example: codium-1.107.18627-el8.x86_64.rpm
            // Pattern: name-version-release.arch.rpm
            // Remove .rpm extension first
            QString nameWithoutExt = baseName;
            if (nameWithoutExt.endsWith(".rpm", Qt::CaseInsensitive)) {
                nameWithoutExt.chop(4);
            }
            
            // Try to find architecture suffix (last part before .rpm)
            // Pattern: something.x86_64, something.noarch, etc.
            QRegularExpression archRegex(R"((\.(x86_64|i686|i386|aarch64|arm64|noarch))$)");
            QRegularExpressionMatch archMatch = archRegex.match(nameWithoutExt);
            QString nameWithoutArch = nameWithoutExt;
            if (archMatch.hasMatch()) {
                nameWithoutArch = nameWithoutExt.left(archMatch.capturedStart());
            }
            
            // Now try to split package-version-release
            // Find the last occurrence of pattern like -el8, -fc38, etc. (release)
            QRegularExpression releaseRegex(R"(-(el\d+|fc\d+|rhel\d+|sles\d+)$)");
            QRegularExpressionMatch releaseMatch = releaseRegex.match(nameWithoutArch);
            QString nameWithoutRelease = nameWithoutArch;
            if (releaseMatch.hasMatch()) {
                nameWithoutRelease = nameWithoutArch.left(releaseMatch.capturedStart());
            }
            
            // Now split package-version
            // Find the last dash that separates package name from version
            // Version typically starts with a digit
            int lastDash = -1;
            for (int i = nameWithoutRelease.length() - 1; i >= 0; i--) {
                if (nameWithoutRelease[i] == '-') {
                    // Check if the part after dash starts with a digit (version)
                    QString afterDash = nameWithoutRelease.mid(i + 1);
                    if (!afterDash.isEmpty() && afterDash[0].isDigit()) {
                        lastDash = i;
                        break;
                    }
                }
            }
            
            if (lastDash > 0) {
                m_metadata.package = nameWithoutRelease.left(lastDash);
                m_metadata.version = nameWithoutRelease.mid(lastDash + 1);
                if (releaseMatch.hasMatch()) {
                    m_metadata.version += "-" + releaseMatch.captured(1); // Add release back with dash
                }
                emit log(QString("Extracted from filename: %1 %2").arg(m_metadata.package).arg(m_metadata.version));
            } else {
                // Simple fallback: split by first dash
                int firstDash = baseName.indexOf('-');
                if (firstDash > 0) {
                    m_metadata.package = baseName.left(firstDash);
                    QString rest = baseName.mid(firstDash + 1);
                    // Remove .rpm and arch suffix
                    rest.remove(QRegularExpression(R"(\.[^.]+\.rpm?$)"));
                    m_metadata.version = rest;
                    emit log(QString("Extracted from filename (simple): %1 %2").arg(m_metadata.package).arg(m_metadata.version));
                }
            }
        }
        
        // Parse metadata from extracted files (may fill in missing info)
        PackageMetadata extractedMetadata = m_rpmParser->parseMetadata(m_extractedPackageDir);
        
        // Merge metadata - prefer RPM query results, but use extracted if RPM query failed
        if (m_metadata.package.isEmpty()) {
            m_metadata = extractedMetadata;
            emit log("Using metadata from extracted files");
        } else {
            // Use extracted metadata for executables, icons, etc.
            m_metadata.executables = extractedMetadata.executables;
            m_metadata.scripts = extractedMetadata.scripts;
            m_metadata.iconPath = extractedMetadata.iconPath;
            m_metadata.mainExecutable = extractedMetadata.mainExecutable;
            if (m_metadata.description.isEmpty()) {
                m_metadata.description = extractedMetadata.description;
            }
            emit log(QString("Merged metadata: %1 executables found").arg(m_metadata.executables.size()));
        }
    } else {
        emit log("ERROR: Unknown package type");
        return false;
    }
    emit log(QString("Package: %1, Version: %2").arg(m_metadata.package).arg(m_metadata.version));
    m_packageProfile = PackageClassifier::classify(
        m_packagePath,
        m_packageType,
        m_extractedPackageDir,
        m_metadata);
    m_conversionPlan = PackageClassifier::buildPlan(m_packageProfile);
    emit log(QString("Package classification: %1").arg(m_packageProfile.summary()));
    emit log(QString("Selected conversion plan: %1").arg(m_conversionPlan.summary()));

    if (m_packageProfile.likelyUnsupported) {
        emit log("WARNING: Package classification indicates a partial-support or service-style package.");
    }
    
    if (!m_metadata.scripts.isEmpty()) {
        emit log("WARNING: Package contains installation scripts (postinst/prerm). These will NOT be executed.");
        for (const QString& script : m_metadata.scripts) {
            emit log(QString("  - %1").arg(QFileInfo(script).fileName()));
        }
    }
    
    if (m_metadata.executables.isEmpty()) {
        emit log("ERROR: No executables found in package");
        emit log("Searched in: /usr/bin, /usr/sbin, /opt, /bin, /sbin, /usr/games, /usr/lib, /usr/libexec, /usr/share");
        
        // For RPM, try to list files in the package to help debug
        if (m_packageType == PackageFormat::Rpm) {
            emit log("Attempting to list files in RPM package...");
            ProcessResult rpmList = SubprocessWrapper::execute("rpm", {
                "-qpl", m_packagePath
            });
            if (rpmList.success && !rpmList.stdoutOutput.isEmpty()) {
                QStringList files = rpmList.stdoutOutput.split('\n', Qt::SkipEmptyParts);
                emit log(QString("RPM contains %1 files. Looking for executables...").arg(files.size()));
                int execCount = 0;
                for (const QString& file : files) {
                    if (file.contains("/bin/") || file.contains("/sbin/") || 
                        file.contains("/opt/") || file.contains("/usr/libexec/") ||
                        (file.startsWith("/usr/") && (file.endsWith(".sh") || file.endsWith(".jar")))) {
                        emit log(QString("  Found potential executable: %1").arg(file));
                        execCount++;
                        if (execCount > 10) {
                            emit log("  ... (showing first 10)");
                            break;
                        }
                    }
                }
            } else {
                // Fallback: list files in extracted directory
                emit log("rpm command not available, listing extracted files...");
                QDir extractedDir(m_extractedPackageDir);
                if (extractedDir.exists()) {
                    QStringList entries = extractedDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
                    emit log(QString("Extracted directory structure: %1").arg(entries.join(", ")));
                    
                    // Check data subdirectory
                    QDir dataDir(QString("%1/data").arg(m_extractedPackageDir));
                    if (dataDir.exists()) {
                        QStringList dataEntries = dataDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
                        emit log(QString("Data directory structure: %1").arg(dataEntries.isEmpty() ? "(empty)" : dataEntries.join(", ")));
                        
                        // Also check if files are directly in data/ (RPM structure)
                        QStringList dataFiles = dataDir.entryList(QDir::Files, QDir::Name);
                        if (!dataFiles.isEmpty()) {
                            emit log(QString("Files directly in data/: %1").arg(dataFiles.mid(0, 10).join(", ")));
                        }
                        
                        // List some files in common directories
                        QStringList checkDirs = {"usr/bin", "usr/sbin", "opt", "bin", "sbin", "usr/lib", "usr/libexec"};
                        for (const QString& checkDir : checkDirs) {
                            QDir dir(dataDir.absoluteFilePath(checkDir));
                            if (dir.exists()) {
                                QStringList files = dir.entryList(QDir::Files, QDir::Name);
                                if (!files.isEmpty()) {
                                    emit log(QString("  Files in %1: %2").arg(checkDir).arg(files.mid(0, 10).join(", ")));
                                }
                            }
                        }
                        
                        // Recursively search for executables
                        emit log("Recursively searching for executables...");
                        QDirIterator it(dataDir.absolutePath(), QDirIterator::Subdirectories);
                        int execCount = 0;
                        while (it.hasNext() && execCount < 20) {
                            QString filePath = it.next();
                            QFileInfo fileInfo(filePath);
                            if (fileInfo.isFile() && fileInfo.isExecutable()) {
                                emit log(QString("  Found executable: %1").arg(filePath.mid(dataDir.absolutePath().length() + 1)));
                                execCount++;
                            } else if (fileInfo.isFile() && (fileInfo.suffix() == "jar" || fileInfo.suffix() == "sh")) {
                                emit log(QString("  Found potential executable: %1").arg(filePath.mid(dataDir.absolutePath().length() + 1)));
                                execCount++;
                            }
                        }
                    } else {
                        // Check if files are directly in extracted directory (not in data/)
                        emit log("Data directory does not exist, checking root extraction directory...");
                        QStringList rootEntries = extractedDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
                        emit log(QString("Root directory entries: %1").arg(rootEntries.join(", ")));
                        
                        // Check common directories directly
                        QStringList checkDirs = {"usr/bin", "usr/sbin", "opt", "bin", "sbin"};
                        for (const QString& checkDir : checkDirs) {
                            QDir dir(extractedDir.absoluteFilePath(checkDir));
                            if (dir.exists()) {
                                QStringList files = dir.entryList(QDir::Files, QDir::Name);
                                if (!files.isEmpty()) {
                                    emit log(QString("  Files in %1: %2").arg(checkDir).arg(files.mid(0, 10).join(", ")));
                                }
                            }
                        }
                    }
                }
            }
        }
        
        // Check if this is a data package
        QString packageName = m_metadata.package.toLower();
        if (packageName.contains("-data") || packageName.contains("-common") || 
            packageName.contains("-dev") || packageName.contains("-doc")) {
            emit log("");
            emit log("TIP: This appears to be a data/common package, not the main application package.");
            emit log("     Data packages contain resources (icons, translations, data files) but no executables.");
            emit log("     You need to convert the MAIN package instead:");
            emit log(QString("     - For GIMP: use 'gimp' or 'gimp-bin' package, not 'gimp-data'"));
            emit log("     - For other apps: look for packages without '-data', '-common', '-dev', '-doc' suffixes");
        } else {
            emit log("Tip: The package might contain only libraries or data files, not executables");
        }
        return false;
    }
    
    emit log(QString("Found %1 executable(s)").arg(m_metadata.executables.size()));
    for (const QString& exec : m_metadata.executables) {
        emit log(QString("  - %1").arg(exec));
    }
    return true;
}

bool PackageToAppImagePipeline::analyzeDependencies() {
    m_libraries = m_analyzer->collectLibraries(m_metadata.executables);
    emit log(QString("Found %1 library dependencies").arg(m_libraries.size()));
    
    // Check for system dependencies
    QStringList warnings = m_analyzer->checkSystemDependencies(m_metadata.depends);
    for (const QString& warning : warnings) {
        emit log(QString("WARNING: %1").arg(warning));
    }
    
    // Resolve and download missing dependencies if enabled
    if (m_dependencySettings.enabled && !m_metadata.depends.isEmpty()) {
        emit log("Resolving package dependencies...");
        m_dependencyResolver->setSettings(m_dependencySettings);
        
        QList<ResolvedDependency> resolved = m_dependencyResolver->resolveDependencies(
            m_metadata.depends, m_appDirPath);
        
        // Add resolved libraries to the list
        QStringList additionalLibs = m_dependencyResolver->getResolvedLibraries();
        if (!additionalLibs.isEmpty()) {
            emit log(QString("Added %1 libraries from dependencies").arg(additionalLibs.size()));
            m_libraries.append(additionalLibs);
        }
    }
    
    return true;
}

bool PackageToAppImagePipeline::buildAppDir() {
    m_appDirPath = QString("%1/AppDir").arg(m_tempDir);
    
    emit log(QString("Building AppDir at: %1").arg(m_appDirPath));
    emit log(QString("Copying %1 executable(s)").arg(m_metadata.executables.size()));
    emit log(QString("Copying %1 library(ies)").arg(m_libraries.size()));
    
    if (!m_appDirBuilder->buildAppDir(m_appDirPath, m_extractedPackageDir, m_metadata, m_libraries)) {
        emit log("ERROR: Failed to build AppDir");
        emit log("Check the logs above for specific error details");
        return false;
    }
    
    emit log("Successfully built AppDir structure");
    
    // Verify .desktop file exists
    QString desktopDir = QString("%1/usr/share/applications").arg(m_appDirPath);
    QDir desktopDirObj(desktopDir);
    if (desktopDirObj.exists()) {
        QStringList desktopFiles = desktopDirObj.entryList({"*.desktop"}, QDir::Files);
        if (desktopFiles.isEmpty()) {
            emit log("WARNING: No .desktop files found after building AppDir!");
            emit log(QString("Checked directory: %1").arg(desktopDir));
        } else {
            emit log(QString("Verified .desktop file(s) exist: %1").arg(desktopFiles.join(", ")));
            for (const QString& file : desktopFiles) {
                QString fullPath = desktopDirObj.absoluteFilePath(file);
                QFileInfo info(fullPath);
                emit log(QString("  - %1 (size: %2 bytes)").arg(file).arg(info.size()));
            }
        }
    } else {
        emit log(QString("ERROR: Desktop directory does not exist: %1").arg(desktopDir));
        return false;
    }
    
    return true;
}

bool PackageToAppImagePipeline::buildAppImage() {
    if (m_outputPath.isEmpty()) {
        QFileInfo packageInfo(m_packagePath);
        QString baseName = packageInfo.baseName();
        m_outputPath = QString("%1/%2.AppImage").arg(QDir::homePath()).arg(baseName);
    }
    
    emit log(QString("Building AppImage: %1").arg(m_outputPath));
    
    if (!m_appImageBuilder->buildAppImage(m_appDirPath, m_outputPath)) {
        emit log("ERROR: Failed to build AppImage.");
        // The error details are already logged by AppImageBuilder
        return false;
    }
    
    emit log(QString("Successfully created AppImage: %1").arg(m_outputPath));
    return true;
}

void PackageToAppImagePipeline::cleanup() {
    if (!m_tempDir.isEmpty() && QDir(m_tempDir).exists()) {
        emit log("Cleaning up temporary files...");
        SubprocessWrapper::removeDirectory(m_tempDir);
    }
}
