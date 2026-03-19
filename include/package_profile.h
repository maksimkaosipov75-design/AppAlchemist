#ifndef PACKAGE_PROFILE_H
#define PACKAGE_PROFILE_H

#include "debparser.h"
#include <QString>
#include <QStringList>

enum class PackageFormat {
    Deb,
    Rpm,
    Tarball,
    Unknown
};

enum class ApplicationProfile {
    Unknown,
    NativeDesktop,
    NativeCli,
    Electron,
    Java,
    Python,
    Script,
    Chrome,
    SelfContainedTarball,
    Service
};

enum class ConversionMode {
    LegacyFallback,
    FastPathPreferred,
    RepairFallback
};

struct PackageProfile {
    PackageFormat format = PackageFormat::Unknown;
    ApplicationProfile applicationProfile = ApplicationProfile::Unknown;
    QString packagePath;
    QString packageName;
    QString mainExecutable;
    QString workingDirectoryHint;
    QStringList indicators;
    bool desktopEntryPresent = false;
    bool isGraphical = false;
    bool supportsFastPath = false;
    bool likelyRequiresRepair = false;
    bool likelyUnsupported = false;

    QString summary() const;
};

struct ConversionPlan {
    ConversionMode initialMode = ConversionMode::LegacyFallback;
    bool allowFallback = true;
    bool enableLaunchProbe = false;
    QStringList reasons;

    QString summary() const;
};

class PackageClassifier {
public:
    static PackageProfile classify(const QString& packagePath,
                                   PackageFormat format,
                                   const QString& extractedDir,
                                   const PackageMetadata& metadata);

    static QString formatToString(PackageFormat format);
    static QString applicationProfileToString(ApplicationProfile profile);
    static QString conversionModeToString(ConversionMode mode);
    static ConversionPlan buildPlan(const PackageProfile& profile);

private:
    static QString resolvePrimaryExecutable(const QString& extractedDir,
                                            const PackageMetadata& metadata);
    static bool containsDesktopEntry(const QString& extractedDir);
    static bool hasSelfContainedLaunchFiles(const QString& extractedDir);
    static bool isLikelyServicePackage(const QString& extractedDir,
                                       const PackageMetadata& metadata);
    static bool isLikelyCliOnly(const QString& mainExecutable,
                                bool desktopEntryPresent);
};

#endif // PACKAGE_PROFILE_H
