#include "package_profile.h"
#include "appdetector.h"
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QRegularExpression>

QString PackageProfile::summary() const {
    QStringList parts;
    parts << QString("format=%1").arg(PackageClassifier::formatToString(format));
    parts << QString("profile=%1").arg(PackageClassifier::applicationProfileToString(applicationProfile));

    if (format == PackageFormat::Tarball && tarballSubtype != TarballSubtype::None) {
        parts << QString("tarball=%1").arg(PackageClassifier::tarballSubtypeToString(tarballSubtype));
    }

    if (!mainExecutable.isEmpty()) {
        parts << QString("main=%1").arg(mainExecutable);
    }

    parts << QString("desktop=%1").arg(desktopEntryPresent ? "yes" : "no");
    parts << QString("graphical=%1").arg(isGraphical ? "yes" : "no");
    parts << QString("fast-path=%1").arg(supportsFastPath ? "yes" : "no");

    if (likelyRequiresRepair) {
        parts << "repair=likely";
    }

    if (likelyUnsupported) {
        parts << "support=partial";
    }

    if (!indicators.isEmpty()) {
        parts << QString("indicators=%1").arg(indicators.join(","));
    }

    return parts.join(" | ");
}

QString ConversionPlan::summary() const {
    QStringList parts;
    parts << QString("mode=%1").arg(PackageClassifier::conversionModeToString(initialMode));
    parts << QString("fallback=%1").arg(allowFallback ? "enabled" : "disabled");
    parts << QString("probe=%1").arg(enableLaunchProbe ? "enabled" : "disabled");

    if (!reasons.isEmpty()) {
        parts << QString("reasons=%1").arg(reasons.join(","));
    }

    return parts.join(" | ");
}

PackageProfile PackageClassifier::classify(const QString& packagePath,
                                           PackageFormat format,
                                           const QString& extractedDir,
                                           const PackageMetadata& metadata) {
    PackageProfile profile;
    profile.format = format;
    profile.packagePath = packagePath;
    profile.packageName = metadata.package;
    profile.mainExecutable = resolvePrimaryExecutable(extractedDir, metadata);
    profile.desktopEntryPresent = containsDesktopEntry(extractedDir);
    if (format == PackageFormat::Tarball) {
        profile.tarballSubtype = detectTarballSubtype(extractedDir,
                                                      metadata,
                                                      profile.desktopEntryPresent,
                                                      profile.mainExecutable);
        switch (profile.tarballSubtype) {
        case TarballSubtype::PortableBinary:
            profile.indicators << "portable-tarball";
            break;
        case TarballSubtype::InstallerArchive:
            profile.indicators << "installer-tarball";
            break;
        case TarballSubtype::SourceArchive:
            profile.indicators << "source-tarball";
            break;
        case TarballSubtype::SelfContainedAppBundle:
            profile.indicators << "app-bundle";
            break;
        case TarballSubtype::Unknown:
            profile.indicators << "tarball-unknown";
            break;
        case TarballSubtype::None:
            break;
        }
    }

    if (isLikelyServicePackage(extractedDir, metadata)) {
        profile.applicationProfile = ApplicationProfile::Service;
        profile.likelyUnsupported = true;
        profile.indicators << "service";
        return profile;
    }

    if (format == PackageFormat::Tarball &&
        (profile.tarballSubtype == TarballSubtype::PortableBinary ||
         profile.tarballSubtype == TarballSubtype::SelfContainedAppBundle ||
         hasSelfContainedLaunchFiles(extractedDir))) {
        profile.applicationProfile = ApplicationProfile::SelfContainedTarball;
        profile.supportsFastPath = true;
        profile.isGraphical = profile.desktopEntryPresent;
        if (profile.tarballSubtype == TarballSubtype::PortableBinary) {
            profile.indicators << "portable-layout";
        }
    }

    if (!profile.mainExecutable.isEmpty() && QFileInfo::exists(profile.mainExecutable)) {
        const AppInfo appInfo = AppDetector::detectApp(extractedDir, extractedDir, profile.mainExecutable, metadata);
        profile.workingDirectoryHint = appInfo.workingDir;

        switch (appInfo.type) {
        case AppType::Electron:
            profile.applicationProfile = ApplicationProfile::Electron;
            profile.likelyRequiresRepair = true;
            profile.indicators << "electron";
            break;
        case AppType::Java:
            profile.applicationProfile = ApplicationProfile::Java;
            profile.likelyRequiresRepair = true;
            profile.indicators << "java";
            break;
        case AppType::Python:
            profile.applicationProfile = ApplicationProfile::Python;
            profile.likelyRequiresRepair = true;
            profile.indicators << "python";
            break;
        case AppType::Script:
            profile.applicationProfile = ApplicationProfile::Script;
            profile.supportsFastPath = true;
            profile.indicators << "script";
            break;
        case AppType::Chrome:
            profile.applicationProfile = ApplicationProfile::Chrome;
            profile.likelyRequiresRepair = true;
            profile.indicators << "chrome";
            break;
        case AppType::Native:
            if (isLikelyCliOnly(profile.mainExecutable, profile.desktopEntryPresent)) {
                profile.applicationProfile = ApplicationProfile::NativeCli;
                profile.indicators << "cli";
            } else {
                profile.applicationProfile = ApplicationProfile::NativeDesktop;
                profile.indicators << "native";
            }
            profile.supportsFastPath = true;
            break;
        case AppType::Unknown:
            break;
        }
    }

    if (profile.applicationProfile == ApplicationProfile::Unknown) {
        if (format == PackageFormat::Tarball &&
            (profile.tarballSubtype == TarballSubtype::SourceArchive ||
             profile.tarballSubtype == TarballSubtype::InstallerArchive)) {
            profile.likelyUnsupported = true;
        }
        if (isLikelyCliOnly(profile.mainExecutable, profile.desktopEntryPresent)) {
            profile.applicationProfile = ApplicationProfile::NativeCli;
            profile.supportsFastPath = true;
            profile.indicators << "cli";
        } else if (profile.desktopEntryPresent) {
            profile.applicationProfile = ApplicationProfile::NativeDesktop;
            profile.supportsFastPath = true;
            profile.indicators << "desktop-entry";
        }
    }

    profile.isGraphical = profile.desktopEntryPresent
        || profile.applicationProfile == ApplicationProfile::Electron
        || profile.applicationProfile == ApplicationProfile::Chrome
        || profile.applicationProfile == ApplicationProfile::NativeDesktop
        || profile.applicationProfile == ApplicationProfile::SelfContainedTarball;

    if (profile.applicationProfile == ApplicationProfile::Unknown) {
        profile.likelyUnsupported = true;
        profile.indicators << "unclassified";
    }

    return profile;
}

QString PackageClassifier::formatToString(PackageFormat format) {
    switch (format) {
    case PackageFormat::Deb:
        return "deb";
    case PackageFormat::Rpm:
        return "rpm";
    case PackageFormat::Tarball:
        return "tarball";
    case PackageFormat::Unknown:
        return "unknown";
    }

    return "unknown";
}

QString PackageClassifier::tarballSubtypeToString(TarballSubtype subtype) {
    switch (subtype) {
    case TarballSubtype::None:
        return "none";
    case TarballSubtype::PortableBinary:
        return "portable-binary";
    case TarballSubtype::InstallerArchive:
        return "installer-archive";
    case TarballSubtype::SourceArchive:
        return "source-archive";
    case TarballSubtype::SelfContainedAppBundle:
        return "self-contained-app-bundle";
    case TarballSubtype::Unknown:
        return "unknown";
    }

    return "unknown";
}

QString PackageClassifier::applicationProfileToString(ApplicationProfile profile) {
    switch (profile) {
    case ApplicationProfile::Unknown:
        return "unknown";
    case ApplicationProfile::NativeDesktop:
        return "native-desktop";
    case ApplicationProfile::NativeCli:
        return "native-cli";
    case ApplicationProfile::Electron:
        return "electron";
    case ApplicationProfile::Java:
        return "java";
    case ApplicationProfile::Python:
        return "python";
    case ApplicationProfile::Script:
        return "script";
    case ApplicationProfile::Chrome:
        return "chrome";
    case ApplicationProfile::SelfContainedTarball:
        return "portable-tarball";
    case ApplicationProfile::Service:
        return "service";
    }

    return "unknown";
}

QString PackageClassifier::conversionModeToString(ConversionMode mode) {
    switch (mode) {
    case ConversionMode::LegacyFallback:
        return "legacy-fallback";
    case ConversionMode::FastPathPreferred:
        return "fast-path-preferred";
    case ConversionMode::RepairFallback:
        return "repair-fallback";
    }

    return "legacy-fallback";
}

ConversionPlan PackageClassifier::buildPlan(const PackageProfile& profile) {
    ConversionPlan plan;

    if (profile.likelyUnsupported) {
        plan.initialMode = ConversionMode::LegacyFallback;
        if (profile.format == PackageFormat::Tarball &&
            profile.tarballSubtype == TarballSubtype::SourceArchive) {
            plan.reasons << "source-tarball";
        } else if (profile.format == PackageFormat::Tarball &&
                   profile.tarballSubtype == TarballSubtype::InstallerArchive) {
            plan.reasons << "installer-tarball";
        } else {
            plan.reasons << "unsupported-profile";
        }
        return plan;
    }

    if (profile.applicationProfile == ApplicationProfile::Electron ||
        profile.applicationProfile == ApplicationProfile::Java ||
        profile.applicationProfile == ApplicationProfile::Python ||
        profile.applicationProfile == ApplicationProfile::Chrome) {
        plan.initialMode = ConversionMode::RepairFallback;
        plan.allowFallback = true;
        plan.enableLaunchProbe = true;
        plan.reasons << "runtime-repair-profile";
        return plan;
    }

    if (profile.supportsFastPath) {
        plan.initialMode = ConversionMode::FastPathPreferred;
        plan.allowFallback = true;
        plan.enableLaunchProbe = profile.isGraphical;
        plan.reasons << "fast-path-candidate";
        return plan;
    }

    plan.initialMode = ConversionMode::LegacyFallback;
    plan.reasons << "default-legacy";
    return plan;
}

QString PackageClassifier::resolvePrimaryExecutable(const QString& extractedDir,
                                                    const PackageMetadata& metadata) {
    if (!metadata.mainExecutable.isEmpty()) {
        return metadata.mainExecutable;
    }

    if (!metadata.executables.isEmpty()) {
        return metadata.executables.first();
    }

    const QString appRunPath = QDir(extractedDir).absoluteFilePath("AppRun");
    if (QFileInfo::exists(appRunPath) && QFileInfo(appRunPath).isExecutable()) {
        return appRunPath;
    }

    return QString();
}

bool PackageClassifier::containsDesktopEntry(const QString& extractedDir) {
    const QStringList desktopDirs = {
        QDir(extractedDir).absoluteFilePath("data/usr/share/applications"),
        QDir(extractedDir).absoluteFilePath("usr/share/applications"),
        QDir(extractedDir).absoluteFilePath("share/applications")
    };

    for (const QString& path : desktopDirs) {
        QDir dir(path);
        if (!dir.exists()) {
            continue;
        }

        if (!dir.entryList({"*.desktop"}, QDir::Files).isEmpty()) {
            return true;
        }
    }

    return false;
}

bool PackageClassifier::hasSelfContainedLaunchFiles(const QString& extractedDir) {
    const QDir dir(extractedDir);
    const QStringList candidates = {
        "AppRun",
        "AppImage",
        "run.sh",
        "start.sh"
    };

    for (const QString& candidate : candidates) {
        const QFileInfo info(dir.absoluteFilePath(candidate));
        if (info.exists() && (info.isExecutable() || info.suffix().compare("sh", Qt::CaseInsensitive) == 0)) {
            return true;
        }
    }

    QDirIterator it(extractedDir, {"*.AppImage"}, QDir::Files, QDirIterator::Subdirectories);
    return it.hasNext();
}

bool PackageClassifier::isLikelyServicePackage(const QString& extractedDir,
                                               const PackageMetadata& metadata) {
    if (metadata.package.contains("daemon", Qt::CaseInsensitive) ||
        metadata.package.contains("service", Qt::CaseInsensitive)) {
        return true;
    }

    const QStringList serviceDirs = {
        QDir(extractedDir).absoluteFilePath("data/usr/lib/systemd/system"),
        QDir(extractedDir).absoluteFilePath("usr/lib/systemd/system"),
        QDir(extractedDir).absoluteFilePath("etc/systemd/system")
    };

    for (const QString& path : serviceDirs) {
        QDir dir(path);
        if (dir.exists() && !dir.entryList({"*.service", "*.socket"}, QDir::Files).isEmpty()) {
            return true;
        }
    }

    return false;
}

TarballSubtype PackageClassifier::detectTarballSubtype(const QString& extractedDir,
                                                       const PackageMetadata& metadata,
                                                       bool desktopEntryPresent,
                                                       const QString& mainExecutable) {
    const QString dataDir = QDir(extractedDir).absoluteFilePath("data");
    const QStringList appBundleMarkers = {
        QDir(dataDir).absoluteFilePath("AppRun"),
        QDir(dataDir).absoluteFilePath(".DirIcon")
    };
    for (const QString& marker : appBundleMarkers) {
        if (QFileInfo::exists(marker)) {
            return TarballSubtype::SelfContainedAppBundle;
        }
    }

    QDirIterator appImageIt(dataDir, {"*.AppImage"}, QDir::Files, QDirIterator::Subdirectories);
    if (appImageIt.hasNext()) {
        return TarballSubtype::SelfContainedAppBundle;
    }

    const QStringList installerPatterns = {
        "install.sh", "installer.sh", "setup.sh", "install", "setup", "*.run"
    };
    for (const QString& pattern : installerPatterns) {
        QDirIterator it(dataDir, {pattern}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            const QFileInfo info(path);
            const QString lowerName = info.fileName().toLower();
            if (lowerName == "install" && !info.isExecutable()) {
                continue;
            }
            return TarballSubtype::InstallerArchive;
        }
    }

    const QStringList sourcePatterns = {
        "CMakeLists.txt", "configure", "configure.ac", "meson.build",
        "pyproject.toml", "setup.py", "Cargo.toml", "Makefile"
    };
    bool hasSourceMarkers = false;
    for (const QString& pattern : sourcePatterns) {
        QDirIterator it(dataDir, {pattern}, QDir::Files, QDirIterator::Subdirectories);
        if (it.hasNext()) {
            hasSourceMarkers = true;
            break;
        }
    }

    const bool hasSourceDirs = QDir(dataDir).exists("src")
        || QDir(dataDir).exists("include")
        || QDir(dataDir).exists("cmake")
        || QDir(dataDir).exists(".git");

    if ((hasSourceMarkers || hasSourceDirs) &&
        metadata.executables.isEmpty() &&
        !desktopEntryPresent &&
        mainExecutable.isEmpty()) {
        return TarballSubtype::SourceArchive;
    }

    if (hasSelfContainedLaunchFiles(extractedDir) ||
        (!metadata.executables.isEmpty() && !desktopEntryPresent)) {
        return TarballSubtype::PortableBinary;
    }

    return TarballSubtype::Unknown;
}

bool PackageClassifier::isLikelyCliOnly(const QString& mainExecutable,
                                        bool desktopEntryPresent) {
    if (mainExecutable.isEmpty() || desktopEntryPresent) {
        return false;
    }

    return mainExecutable.contains("/usr/bin/")
        || mainExecutable.contains("/bin/")
        || mainExecutable.contains("/usr/sbin/")
        || mainExecutable.contains("/sbin/");
}
