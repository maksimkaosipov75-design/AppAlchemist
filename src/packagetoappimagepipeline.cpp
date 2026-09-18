#include "packagetoappimagepipeline.h"
#include "rpm_repository.h"
#include "rpmparser.h"
#include "appdetector.h"
#include "utils.h"
#include <QStandardPaths>
#include <QDir>
#include <QDirIterator>
#include <QDebug>
#include <QDateTime>
#include <QFileInfo>
#include <QFile>
#include <QMetaObject>
#include <QRegularExpression>
#include <QTemporaryDir>

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
    , m_packageExtractor(new PackageExtractor(m_debParser, m_rpmParser, m_tarballParser))
    , m_packageInspector(new PackageInspector())
    , m_packagePackager(new PackagePackager(m_appDirBuilder, m_appImageBuilder))
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
    delete m_packageExtractor;
    delete m_packageInspector;
    delete m_packagePackager;
    // QObject children are deleted by QObject parent ownership.
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

void PackageToAppImagePipeline::setStopToken(std::stop_token token) {
    m_stopToken = token;
}

bool PackageToAppImagePipeline::isCancelled() const {
    if (m_cancelled.load()) {
        return true;
    }
    if (m_stopToken.has_value() && m_stopToken->stop_requested()) {
        return true;
    }
    return false;
}

void PackageToAppImagePipeline::setDryRun(bool dryRun) {
    m_dryRun = dryRun;
}

bool PackageToAppImagePipeline::isDryRun() const {
    return m_dryRun;
}

void PackageToAppImagePipeline::start() {
    if (!isCancelled()) {
        m_cancelled.store(false);
    }
    QMetaObject::invokeMethod(this, "process", Qt::QueuedConnection);
}

void PackageToAppImagePipeline::cancel() {
    m_cancelled.store(true);
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
    
    // Create secure temp directory with 0700 permissions
    QTemporaryDir tempDir(QDir::tempPath() + "/appalchemist-XXXXXX");
    tempDir.setAutoRemove(false);
    if (!tempDir.isValid()) {
        emit error("Failed to create temporary directory");
        emit finished();
        return;
    }
    m_tempDir = tempDir.path();
    QFile::setPermissions(m_tempDir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    
    emit log(QString("Using temporary directory: %1").arg(m_tempDir));
    
    // Step 1: Validate input
    if (isCancelled()) {
        cleanup();
        emit finished();
        return;
    }
    
    emit progress(10, QString("Validating %1 file...").arg(packageTypeStr));
    if (!validateInput()) {
        if (isCancelled()) {
            cleanup();
            emit finished();
            return;
        }
        emit error(QString("Invalid %1 file").arg(packageTypeStr));
        cleanup();
        emit finished();
        return;
    }
    
    // Step 2: Extract .deb
    if (isCancelled()) {
        cleanup();
        emit finished();
        return;
    }
    
    emit progress(20, QString("Extracting %1 package...").arg(packageTypeStr));
    if (!extractPackage()) {
        if (isCancelled()) {
            cleanup();
            emit finished();
            return;
        }
        emit error(QString("Failed to extract %1 package or no executables found").arg(packageTypeStr));
        cleanup();
        emit finished();
        return;
    }
    
    // Step 3: Analyze dependencies
    if (isCancelled()) {
        cleanup();
        emit finished();
        return;
    }
    
    emit progress(40, "Analyzing dependencies...");
    if (!analyzeDependencies()) {
        if (isCancelled()) {
            cleanup();
            emit finished();
            return;
        }
        emit error("Failed to analyze dependencies");
        cleanup();
        emit finished();
        return;
    }

    if (m_dryRun) {
        emit progress(100, "Dry-run validation complete");
        emit log(QString("Dry-run inspection succeeded for %1 (Format: %2, Profile: %3)")
                     .arg(m_packagePath)
                     .arg(packageTypeStr)
                     .arg(PackageClassifier::applicationProfileToString(m_packageProfile.applicationProfile)));
        emit success(m_outputPath.isEmpty() ? m_packagePath + ".AppImage" : m_outputPath);
        cleanup();
        emit finished();
        return;
    }

    if (!executeConversionPlan()) {
        if (isCancelled()) {
            cleanup();
            emit finished();
            return;
        }
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

    if (isCancelled()) {
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

    if (isCancelled()) {
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

    if (isCancelled()) {
        return false;
    }

    bundleAppDirLibraries("Fast path");

    if (isCancelled()) {
        return false;
    }

    if (!optimizeBuiltAppDir("Fast path")) {
        return false;
    }

    if (isCancelled()) {
        return false;
    }

    if (!runRuntimeProbe(mainExec, "Fast path", m_packageProfile.applicationProfile == ApplicationProfile::NativeCli)) {
        return false;
    }

    if (isCancelled()) {
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

    if (isCancelled()) {
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

    bundleAppDirLibraries("Repair path");

    if (isCancelled()) {
        return false;
    }

    if (m_dependencySettings.enabled &&
        !resolveAppDirDependencies(mainExec, "Repair path", true)) {
        return false;
    }

    if (!m_dependencySettings.enabled) {
        const QStringList remaining = findMissingRuntimeLibraries(mainExec);
        if (!remaining.isEmpty() && remaining != QStringList{"__ldd_failed__"}) {
            emit log(QString("Repair path still sees unresolved runtime libraries: %1. "
                             "Enable repository dependency resolution to fetch them.")
                     .arg(remaining.join(", ")));
            return false;
        }
    }

    if (isCancelled()) {
        return false;
    }

    if (!verifyAppDirReadiness(mainExec, m_packageProfile.isGraphical)) {
        return false;
    }

    if (!optimizeBuiltAppDir("Repair path")) {
        return false;
    }

    if (isCancelled()) {
        return false;
    }

    if (!runRuntimeProbe(mainExec, "Repair path", false)) {
        return false;
    }

    if (isCancelled()) {
        return false;
    }

    if (!packageBuiltAppDir("Repair path")) {
        return false;
    }

    emit log("Repair-oriented path completed successfully.");
    return true;
}

bool PackageToAppImagePipeline::executeFallbackPath() {
    if (isCancelled()) {
        return false;
    }

    emit progress(55, "Building AppDir structure...");
    if (!buildAppDir()) {
        emit log("Fallback path failed while building AppDir");
        return false;
    }

    if (isCancelled()) {
        return false;
    }

    const QString mainExec = findPrimaryAppDirExecutable();

    bundleAppDirLibraries("Fallback path");

    if (isCancelled()) {
        return false;
    }

    if (m_dependencySettings.enabled) {
        if (mainExec.isEmpty() || !QFileInfo::exists(mainExec)) {
            emit log("WARNING: Could not find main executable for dependency analysis");
        } else if (!resolveAppDirDependencies(mainExec, "Fallback path", false)) {
            emit log("WARNING: Fallback dependency repair did not fully resolve runtime issues");
        }
    }

    if (isCancelled()) {
        return false;
    }

    if (!optimizeBuiltAppDir("Fallback path")) {
        return false;
    }

    if (isCancelled()) {
        return false;
    }

    if (!runRuntimeProbe(mainExec, "Fallback path", false)) {
        emit log("WARNING: Fallback runtime probe reported issues, continuing to package anyway");
    }

    if (isCancelled()) {
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

QStringList PackageToAppImagePipeline::selectCompanionPackages(const QStringList& depends,
                                                               const QString& packageName,
                                                               bool interpreted) {
    Q_UNUSED(interpreted);
    QStringList companions;
    for (const QString& dependency : depends) {
        // "python3:any" names the package python3: the qualifier says which
        // architecture satisfies it, and is not part of the name.
        const QString name = dependency.section(' ', 0, 0).section(':', 0, 0).trimmed();
        if (name.isEmpty() || companions.contains(name)) {
            continue;
        }
        const bool namedAfterPackage = !packageName.isEmpty() && name.startsWith(packageName + "-");
        const bool looksLikeData = name.endsWith("-common") || name.endsWith("-data");
        // A dependency pinned to one exact version is built from the same
        // source as the package itself: that is how a distribution splits one
        // application across several packages, and the name need say nothing
        // about it - quodlibet keeps its own Python module in "exfalso".
        const bool sameSource = dependency.contains("(= ");
        // An interpreted application is nothing without its modules, and they
        // live in packages of their own. Without them the program starts and
        // dies on its first import.
        const bool interpreterModule = name.startsWith("python3-") ||
                                       name.startsWith("python-") ||
                                       name.startsWith("gir1.2-");
        if (sameSource || namedAfterPackage || looksLikeData || interpreterModule) {
            companions << name;
        }
    }
    return companions;
}

bool PackageToAppImagePipeline::fetchRpmDependencies(const QStringList& missingSonames) {
    if (m_packageType != PackageFormat::Rpm) {
        return false;
    }

    const RpmHeaderInfo header = RpmParser::readRpmHeader(m_packagePath);
    const QString release = RpmRepository::releaseFromRpmTag(header.release);
    if (release.isEmpty()) {
        emit log("The package does not say which release it was built for; "
                 "its dependencies cannot be fetched");
        return false;
    }

    RpmRepository repository;
    connect(&repository, &RpmRepository::log, this, &PackageToAppImagePipeline::log);
    repository.setRelease(release);
    repository.setArchitecture(detectSystemArchitecture());

    if (!repository.ensureMetadata()) {
        return false;
    }

    QSet<QString> sonames;
    for (const QString& soname : missingSonames) {
        sonames.insert(soname);
    }

    // The package's own companions - its data, its private libraries - are
    // named in its requirements alongside the sonames.
    QSet<QString> packageNames;
    for (const QString& requirement : m_metadata.depends) {
        const QString name = requirement.section(' ', 0, 0).trimmed();
        if (name.isEmpty() || name.startsWith('/') || name.contains(".so")) {
            continue;
        }
        packageNames.insert(name);
    }

    const QHash<QString, QString> located = repository.resolve(sonames, packageNames);
    if (located.isEmpty()) {
        return false;
    }

    const QString downloadDir = m_tempDir + "/rpm_dependencies";
    const QStringList packages = repository.download(located.values(), downloadDir);
    if (packages.isEmpty()) {
        return false;
    }

    // Unpack straight into the AppDir: the layout inside an RPM already
    // matches the one the bundle uses.
    RpmParser parser;
    int unpacked = 0;
    for (const QString& package : packages) {
        const QString stage = m_tempDir + "/rpm_stage";
        SubprocessWrapper::removeDirectory(stage);
        QDir().mkpath(stage);
        if (!parser.extractRpm(package, stage)) {
            emit log(QString("Could not unpack %1").arg(QFileInfo(package).fileName()));
            continue;
        }
        const QString payload = QDir(stage).exists("data") ? stage + "/data" : stage;
        if (SubprocessWrapper::copyDirectory(payload, m_appDirPath)) {
            unpacked++;
        }
        SubprocessWrapper::removeDirectory(stage);
    }

    emit log(QString("Bundled %1 packages from the Fedora %2 repository")
                 .arg(unpacked)
                 .arg(release));
    return unpacked > 0;
}

void PackageToAppImagePipeline::bundleAppDirLibraries(const QString& stageLabel) {
    if (!m_dependencySettings.bundleSystemLibraries) {
        emit log(QString("%1 skipped library bundling (disabled in settings).").arg(stageLabel));
        return;
    }

    emit progress(62, "Bundling shared libraries...");
    m_dependencyResolver->setSettings(m_dependencySettings);

    LibraryBundleReport report = m_dependencyResolver->bundleSystemLibraries(m_appDirPath);
    emit log(QString("%1 library bundling: %2").arg(stageLabel, report.summary()));

    // Distributions split an application across packages: the program in one,
    // the libraries it was built with in another. Whatever is not installed on
    // this machine cannot be bundled, and the result would start nowhere else
    // either. Fetch those packages and bundle again rather than shipping a
    // bundle that is known to be broken.
    // Each round of fetching reveals the libraries the newly bundled ones need
    // in turn, so this repeats while it keeps making progress.
    if (m_packageType == PackageFormat::Rpm) {
        for (int round = 0; round < 4 && report.ran && !report.unresolved.isEmpty(); ++round) {
            const int before = report.unresolved.size();
            emit log(QString("%1 has %2 unresolved libraries; fetching them from the distribution "
                             "the package was built for")
                         .arg(stageLabel)
                         .arg(before));
            if (!fetchRpmDependencies(report.unresolved)) {
                break;
            }
            const QStringList relocated =
                m_appDirBuilder->relocatePackagePaths(m_appDirPath, m_metadata);
            Q_UNUSED(relocated);
            report = m_dependencyResolver->bundleSystemLibraries(m_appDirPath);
            emit log(QString("%1 library bundling after fetch: %2").arg(stageLabel, report.summary()));
            if (report.unresolved.size() >= before) {
                break;   // nothing more to gain from another round
            }
        }
        m_triedDependencyFetch = true;
    }

    // Bundling a library reveals what it needs in turn, so a name can become
    // missing only after an earlier fetch: kcalc learns it needs
    // libdbusmenu-qt5 only once the KDE libraries are in place. Each round
    // therefore asks again, and stops as soon as there is nothing new to try.
    for (int round = 0; round < 4; ++round) {
        if (!report.ran || report.unresolved.isEmpty()) {
            break;
        }

        QStringList namedAfterMissing;
        for (const QString& missing : report.unresolved) {
            for (const QString& candidate : DependencyResolver::packageNamesForSoname(missing)) {
                if (!m_attemptedPackages.contains(candidate) &&
                    !namedAfterMissing.contains(candidate)) {
                    namedAfterMissing << candidate;
                }
            }
        }

        const bool firstAttempt = !m_metadata.depends.isEmpty() && !m_triedDependencyFetch;
        if (namedAfterMissing.isEmpty() && !firstAttempt) {
            break;
        }
        m_triedDependencyFetch = true;

        emit log(QString("%1 has %2 unresolved libraries; fetching the packages that provide them")
                     .arg(stageLabel)
                     .arg(report.unresolved.size()));

        DependencySettings fetchSettings = m_dependencySettings;
        fetchSettings.enabled = true;
        m_dependencyResolver->setSettings(fetchSettings);

        const QString fetched = m_tempDir + QString("/fetched_deps_%1").arg(round);
        QDir().mkpath(fetched);

        QStringList toFetch;
        if (firstAttempt) {
            // The library an executable was built against often belongs to a
            // package that the declared dependencies only reach through
            // another one, so the list is expanded before anything is
            // downloaded.
            toFetch = m_metadata.depends;
            const QStringList transitive =
                m_dependencyResolver->expandLibraryDependencies(m_metadata.depends);
            if (!transitive.isEmpty()) {
                emit log(QString("Including %1 library packages reached through other packages")
                             .arg(transitive.size()));
                toFetch << transitive;
            }
        }

        for (const QString& candidate : namedAfterMissing) {
            if (!toFetch.contains(candidate)) {
                toFetch << candidate;
            }
            m_attemptedPackages.insert(candidate);
        }
        m_dependencyResolver->requirePackages(namedAfterMissing);

        m_dependencyResolver->resolveDependencies(toFetch, fetched);

        const bool copied = SubprocessWrapper::copyDirectory(fetched, m_appDirPath);
        m_dependencyResolver->setSettings(m_dependencySettings);
        if (!copied) {
            break;
        }

        emit log("Bundled the packages the application was built against");
        const int before = report.unresolved.size();
        report = m_dependencyResolver->bundleSystemLibraries(m_appDirPath);
        emit log(QString("%1 library bundling after fetch: %2").arg(stageLabel, report.summary()));
        if (report.unresolved.size() >= before && namedAfterMissing.isEmpty()) {
            break;
        }
    }

    if (report.ran && !report.unresolved.isEmpty()) {
        emit log(QString("WARNING: %1 libraries are still missing and the application may not "
                         "start on a machine without them: %2")
                     .arg(report.unresolved.size())
                     .arg(report.unresolved.join(", ").left(200)));
    }

    if (report.ran && !report.bundled.isEmpty()) {
        // Directories that arrived with the bundled libraries - an
        // application's loadable backends live in one - were not in the AppDir
        // when its references were first rewritten, so run that pass again.
        const QStringList lateRelocations =
            m_appDirBuilder->relocatePackagePaths(m_appDirPath, m_metadata);
        if (!lateRelocations.isEmpty()) {
            emit log(QString("Made %1 more package paths bundle-relative after bundling")
                         .arg(lateRelocations.size()));
        }

        // AppRun was written while building the AppDir, before the bundled
        // GLib/GTK module trees existed. Regenerate it so it exports
        // GIO_MODULE_DIR, GDK_PIXBUF_MODULE_FILE and friends.
        if (!m_appDirBuilder->createAppRun(m_appDirPath, m_metadata)) {
            emit log(QString("WARNING: %1 could not refresh AppRun after bundling libraries.")
                     .arg(stageLabel));
        }
    }
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
    // The environment AppRun exports describes what the bundle holds, and the
    // bundle only stops changing here: libraries, their data directories and
    // fetched packages all arrive after the AppDir is first built. Writing it
    // last is what makes the two agree - Guile's startup files were bundled
    // but unnamed, and the interpreter aborted as if they were missing.
    if (!m_appDirBuilder->createAppRun(m_appDirPath, m_metadata)) {
        emit log(QString("WARNING: %1 could not write AppRun with the final bundle contents.")
                     .arg(stageLabel));
    }

    const int removedLinks = DependencyResolver::removeDanglingSymlinks(m_appDirPath);
    if (removedLinks > 0) {
        emit log(QString("%1 removed %2 symlinks that resolved to nothing")
                     .arg(stageLabel)
                     .arg(removedLinks));
    }

    emit progress(85, "Building AppImage...");
    if (!buildAppImage()) {
        emit log(QString("%1 failed while building AppImage.").arg(stageLabel));
        return false;
    }

    return true;
}

bool PackageToAppImagePipeline::runRuntimeProbe(const QString& executablePath,
                                                const QString& stageLabel,
                                                bool requiredForSuccess) {
    emit progress(80, "Running AppDir probe...");

    const RuntimeProbeResult probeResult = RuntimeProbePolicy::probe(m_appDirPath,
                                                                     m_packageProfile,
                                                                     m_metadata,
                                                                     executablePath);

    emit log(QString("%1 runtime probe summary: %2").arg(stageLabel, probeResult.summary()));
    if (!probeResult.commandSummary().isEmpty()) {
        emit log(QString("%1 runtime probe command: %2").arg(stageLabel, probeResult.commandSummary()));
    }

    for (const QString& check : probeResult.checks) {
        emit log(QString("%1 runtime probe check: %2").arg(stageLabel, check));
    }

    for (const QString& warning : probeResult.warnings) {
        emit log(QString("%1 runtime probe warning: %2").arg(stageLabel, warning));
    }

    if (!probeResult.stdoutOutput.isEmpty()) {
        emit log(QString("%1 runtime probe stdout: %2").arg(stageLabel, probeResult.stdoutOutput));
    }

    if (!probeResult.stderrOutput.isEmpty()) {
        emit log(QString("%1 runtime probe stderr: %2").arg(stageLabel, probeResult.stderrOutput));
    }

    if (probeResult.success) {
        emit log(QString("%1 runtime probe passed.").arg(stageLabel));
        return true;
    }

    for (const QString& failure : probeResult.failures) {
        emit log(QString("%1 runtime probe failure: %2").arg(stageLabel, failure));
    }

    return !requiredForSuccess;
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

    // A script has no libraries of its own - the interpreter it names does.
    // Asking the loader about it always fails, and treating that failure as an
    // unresolved dependency used to abandon the conversion of every
    // interpreted application.
    QFile entry(executablePath);
    if (entry.open(QIODevice::ReadOnly)) {
        const QByteArray magic = entry.read(4);
        entry.close();
        if (magic != QByteArray("\x7f""ELF", 4)) {
            return missingLibraries;
        }
    }

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QString existingLdPath = env.value("LD_LIBRARY_PATH");
    env.insert("LD_LIBRARY_PATH", QString("%1/usr/lib:%2").arg(m_appDirPath, existingLdPath));

    const QStringList lines = m_dependencyResolver ? m_dependencyResolver->runSafeLdd(executablePath, env)
                                                   : DependencyResolver::runSafeLdd(executablePath, env);
    if (lines.isEmpty()) {
        missingLibraries << "__ldd_failed__";
        return missingLibraries;
    }
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
    const PackageValidationResult result = m_packageExtractor->validate(m_packagePath, m_packageType);
    for (const QString& line : result.logs) {
        emit log(line);
    }
    return result.success;
}

bool PackageToAppImagePipeline::extractPackage() {
    const PackageExtractionResult extractionResult = m_packageExtractor->extract(m_packagePath, m_packageType, m_tempDir);
    for (const QString& line : extractionResult.logs) {
        emit log(line);
    }
    if (!extractionResult.success) {
        return false;
    }

    m_extractedPackageDir = extractionResult.extractedDir;
    m_metadata = extractionResult.metadata;

    const PackageInspectionResult inspectionResult = m_packageInspector->inspect(
        m_packagePath,
        m_packageType,
        m_extractedPackageDir,
        m_metadata);
    for (const QString& line : inspectionResult.logs) {
        emit log(line);
    }
    if (!inspectionResult.success) {
        return false;
    }

    m_packageProfile = inspectionResult.profile;
    m_conversionPlan = inspectionResult.plan;
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
    
    // Distributions ship an application's data in a companion package:
    // <name>-common, <name>-data and the like hold the settings schemas, the
    // interface definitions and the icons, while the package named after the
    // application holds only the executable. Converting just the latter
    // produces a bundle that starts and immediately fails on its own settings,
    // so these companions are always fetched - they are small and belong to
    // the application in everything but packaging.
    if (!m_metadata.depends.isEmpty()) {
        const QString base = m_metadata.package.trimmed();

        // For an interpreted application the dependency list is the
        // application: its modules, and often its own code, are packaged
        // separately under names that follow no rule. Bundling only the
        // packages whose names look related leaves it unable to import
        // itself, so everything it declares is taken.
        const bool interpreted = AppDetector::isPython(m_metadata.mainExecutable) ||
                                 AppDetector::isPythonLauncherScript(m_metadata.mainExecutable);

        QStringList companions =
            selectCompanionPackages(m_metadata.depends, base, interpreted);

        if (!companions.isEmpty()) {
            // A module package depends on further module packages, and the
            // application fails on the first import that is missing, so the
            // list is followed to its end.
            const QStringList reached = m_dependencyResolver->expandLibraryDependencies(companions);
            for (const QString& name : reached) {
                if (!companions.contains(name)) {
                    companions << name;
                }
            }
            emit log(QString("Fetching companion packages: %1").arg(companions.join(", ").left(160)));
            DependencySettings companionSettings = m_dependencySettings;
            companionSettings.enabled = true;
            m_dependencyResolver->setSettings(companionSettings);

            const QString companionDir = m_tempDir + "/companion_packages";
            QDir().mkpath(companionDir);
            m_dependencyResolver->resolveDependencies(companions, companionDir);

            const QString mergeTarget = QDir(m_extractedPackageDir).exists(QStringLiteral("data"))
                ? m_extractedPackageDir + "/data"
                : m_extractedPackageDir;
            if (SubprocessWrapper::copyDirectory(companionDir, mergeTarget)) {
                emit log("Merged companion package contents");
            }
            m_dependencyResolver->setSettings(m_dependencySettings);
        }
    }

    // Resolve and download missing dependencies if enabled
    if (m_dependencySettings.enabled && !m_metadata.depends.isEmpty()) {
        emit log("Resolving package dependencies...");
        m_dependencyResolver->setSettings(m_dependencySettings);
        
        const QString stagedDepsDir = m_appDirPath.isEmpty() ? (m_tempDir + "/staged_deps") : m_appDirPath;
        QDir().mkpath(stagedDepsDir);
        QList<ResolvedDependency> resolved = m_dependencyResolver->resolveDependencies(
            m_metadata.depends, stagedDepsDir);
        
        // Add resolved libraries to the list
        QStringList additionalLibs = m_dependencyResolver->getResolvedLibraries();
        if (!additionalLibs.isEmpty()) {
            emit log(QString("Added %1 libraries from dependencies").arg(additionalLibs.size()));
            m_libraries.append(additionalLibs);
        }

        // Dependencies bring more than shared libraries: launcher fragments,
        // interpreters, data. A package whose script sources a helper from
        // another package cannot start without it, so merge whatever was
        // fetched into the extracted tree and let the AppDir builder treat it
        // like the package's own content.
        const QString fetchedRoot = m_tempDir + "/staged_deps";
        if (QDir(fetchedRoot).exists() && fetchedRoot != m_appDirPath) {
            const QString mergeTarget = QDir(m_extractedPackageDir).exists(QStringLiteral("data"))
                ? m_extractedPackageDir + "/data"
                : m_extractedPackageDir;
            if (SubprocessWrapper::copyDirectory(fetchedRoot, mergeTarget)) {
                emit log("Merged dependency contents into the bundle");
            }
        }
    }
    
    return true;
}

bool PackageToAppImagePipeline::buildAppDir() {
    const AppDirBuildResult result = m_packagePackager->buildAppDir(
        m_tempDir,
        m_extractedPackageDir,
        m_metadata,
        m_libraries);
    for (const QString& line : result.logs) {
        emit log(line);
    }
    if (!result.success) {
        return false;
    }

    m_appDirPath = result.appDirPath;

    // Recursively copy all staged dependency libraries AND soname symlinks into the final AppDir
    const QString stagedLibDir = m_tempDir + "/staged_deps/usr/lib";
    if (QDir(stagedLibDir).exists()) {
        const QString targetLibDir = m_appDirPath + "/usr/lib";
        QDir().mkpath(targetLibDir);
        emit log("Copying staged dependency libraries and symlinks into AppDir...");
        SubprocessWrapper::copyDirectory(stagedLibDir, targetLibDir);
    }

    return true;
}

bool PackageToAppImagePipeline::buildAppImage() {
    const AppImagePackagingResult result = m_packagePackager->buildAppImage(
        m_packagePath,
        m_appDirPath,
        m_outputPath);
    for (const QString& line : result.logs) {
        emit log(line);
    }
    if (!result.success) {
        return false;
    }

    m_outputPath = result.outputPath;
    return true;
}

void PackageToAppImagePipeline::cleanup() {
    if (!m_tempDir.isEmpty() && QDir(m_tempDir).exists()) {
        emit log("Cleaning up temporary files...");
        SubprocessWrapper::removeDirectory(m_tempDir);
    }
    m_tempDir.clear();
}
