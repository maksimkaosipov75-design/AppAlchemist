#ifndef PACKAGETOAPPIMAGEPIPELINE_H
#define PACKAGETOAPPIMAGEPIPELINE_H

#include <QObject>
#include <QSet>
#include <QString>
#include <QThread>
#include <atomic>
#include <stop_token>
#include <optional>
#include "debparser.h"
#include "rpmparser.h"
#include "tarballparser.h"
#include "dependencyanalyzer.h"
#include "appdirbuilder.h"
#include "appimagebuilder.h"
#include "size_optimizer.h"
#include "dependency_resolver.h"
#include "package_extractor.h"
#include "package_inspector.h"
#include "package_packager.h"
#include "package_profile.h"
#include "runtime_probe.h"

class PackageToAppImagePipeline : public QObject {
    Q_OBJECT

public:
    explicit PackageToAppImagePipeline(QObject* parent = nullptr);
    ~PackageToAppImagePipeline();
    
    void setPackagePath(const QString& packagePath);
    void setOutputPath(const QString& outputPath);
    void setOptimizationSettings(const OptimizationSettings& settings);
    OptimizationSettings optimizationSettings() const;
    void setDependencySettings(const DependencySettings& settings);
    DependencySettings dependencySettings() const;
    void setSudoPassword(const QString& password);
    void start();
    void cancel();
    void setStopToken(std::stop_token token);
    bool isCancelled() const;
    void setDryRun(bool dryRun);
    bool isDryRun() const;

signals:
    void progress(int percentage, const QString& message);
    void log(const QString& message);
    void error(const QString& errorMessage);
    void success(const QString& appImagePath);
    void finished();

private slots:
    void process();

private:
    bool executeConversionPlan();
    bool executeFastPath();
    bool executeRepairPath();
    bool executeFallbackPath();
    bool shouldUseFastPath() const;
    bool shouldUseRepairPath() const;
    void logConversionPlan();
    bool verifyAppDirReadiness(const QString& executablePath, bool requireDesktopEntry) const;
    bool resolveAppDirDependencies(const QString& executablePath, const QString& stageLabel, bool requiredForSuccess);
    // Copies the host shared libraries the AppDir links against into the AppDir
    // so the produced AppImage does not depend on host packages. Runs on every
    // conversion path.
    void bundleAppDirLibraries(const QString& stageLabel);
    bool optimizeBuiltAppDir(const QString& stageLabel);
    bool packageBuiltAppDir(const QString& stageLabel);
    bool runRuntimeProbe(const QString& executablePath, const QString& stageLabel, bool requiredForSuccess);
    QString findPrimaryAppDirExecutable() const;
public:
    // The libraries the given executable links against and the loader cannot
    // find. A script is not an executable in this sense: it has no libraries
    // of its own, and the answer for one is always empty.
    QStringList findMissingRuntimeLibraries(const QString& executablePath) const;

private:

    QString m_packagePath;
    QString m_outputPath;
    QString m_tempDir;
    PackageFormat m_packageType;
    PackageProfile m_packageProfile;
    ConversionPlan m_conversionPlan;
    std::atomic<bool> m_cancelled{false};
    std::optional<std::stop_token> m_stopToken;
    bool m_dryRun{false};
    
    PackageFormat detectPackageType(const QString& packagePath);
    bool validateInput();
    bool extractPackage();
    bool analyzeDependencies();
    bool buildAppDir();
    bool buildAppImage();
    void cleanup();
    
    DebParser* m_debParser;
    RpmParser* m_rpmParser;
    TarballParser* m_tarballParser;
    DependencyAnalyzer* m_analyzer;
    AppDirBuilder* m_appDirBuilder;
    AppImageBuilder* m_appImageBuilder;
    PackageExtractor* m_packageExtractor;
    PackageInspector* m_packageInspector;
    PackagePackager* m_packagePackager;
    SizeOptimizer* m_sizeOptimizer;
    DependencyResolver* m_dependencyResolver;
    OptimizationSettings m_optimizationSettings;
    DependencySettings m_dependencySettings;
    // Fetching the packages that provide missing libraries is attempted once
    // per conversion: a second round would download the same set again.
    bool m_triedDependencyFetch = false;

    // Packages already offered to the package manager, so a second attempt at
    // the same name is not made when a library stays missing because no such
    // package exists.
    QSet<QString> m_attemptedPackages;

public:
    // The packages that carry parts of the application itself rather than
    // system libraries. For an interpreted application that is everything it
    // declares: its own code is routinely split into a package whose name
    // follows no convention (quodlibet keeps its module in "exfalso").
    static QStringList selectCompanionPackages(const QStringList& depends,
                                               const QString& packageName,
                                               bool interpreted);

private:

    // Fetches what an .rpm needs from the distribution it was built for,
    // which the host's package manager cannot provide.
    bool fetchRpmDependencies(const QStringList& missingSonames);
    
    PackageMetadata m_metadata;
    QStringList m_libraries;
    QString m_extractedPackageDir;
    QString m_appDirPath;
};

#endif // PACKAGETOAPPIMAGEPIPELINE_H
