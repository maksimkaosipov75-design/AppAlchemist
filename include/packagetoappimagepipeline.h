#ifndef PACKAGETOAPPIMAGEPIPELINE_H
#define PACKAGETOAPPIMAGEPIPELINE_H

#include <QObject>
#include <QString>
#include <QThread>
#include "debparser.h"
#include "rpmparser.h"
#include "tarballparser.h"
#include "dependencyanalyzer.h"
#include "appdirbuilder.h"
#include "appimagebuilder.h"
#include "size_optimizer.h"
#include "dependency_resolver.h"
#include "package_profile.h"

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
    bool optimizeBuiltAppDir(const QString& stageLabel);
    bool packageBuiltAppDir(const QString& stageLabel);
    bool probeAppDirRuntime(const QString& stageLabel, bool requiredForSuccess);
    bool probeAppRunSyntax(const QString& appRunPath) const;
    QString findPrimaryAppDirExecutable() const;
    QStringList findMissingRuntimeLibraries(const QString& executablePath) const;

    QString m_packagePath;
    QString m_outputPath;
    QString m_tempDir;
    PackageFormat m_packageType;
    PackageProfile m_packageProfile;
    ConversionPlan m_conversionPlan;
    bool m_cancelled;
    
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
    SizeOptimizer* m_sizeOptimizer;
    DependencyResolver* m_dependencyResolver;
    OptimizationSettings m_optimizationSettings;
    DependencySettings m_dependencySettings;
    
    PackageMetadata m_metadata;
    QStringList m_libraries;
    QString m_extractedPackageDir;
    QString m_appDirPath;
};

#endif // PACKAGETOAPPIMAGEPIPELINE_H
