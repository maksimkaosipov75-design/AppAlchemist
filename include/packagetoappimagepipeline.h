#ifndef PACKAGETOAPPIMAGEPIPELINE_H
#define PACKAGETOAPPIMAGEPIPELINE_H

#include <QObject>
#include <QString>
#include <QThread>
#include "debparser.h"
#include "rpmparser.h"
#include "dependencyanalyzer.h"
#include "appdirbuilder.h"
#include "appimagebuilder.h"

enum class PackageType {
    DEB,
    RPM,
    UNKNOWN
};

class PackageToAppImagePipeline : public QObject {
    Q_OBJECT

public:
    explicit PackageToAppImagePipeline(QObject* parent = nullptr);
    ~PackageToAppImagePipeline();
    
    void setPackagePath(const QString& packagePath);
    void setOutputPath(const QString& outputPath);
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
    QString m_packagePath;
    QString m_outputPath;
    QString m_tempDir;
    PackageType m_packageType;
    bool m_cancelled;
    
    PackageType detectPackageType(const QString& packagePath);
    bool validateInput();
    bool extractPackage();
    bool analyzeDependencies();
    bool buildAppDir();
    bool buildAppImage();
    void cleanup();
    
    DebParser* m_debParser;
    RpmParser* m_rpmParser;
    DependencyAnalyzer* m_analyzer;
    AppDirBuilder* m_appDirBuilder;
    AppImageBuilder* m_appImageBuilder;
    
    PackageMetadata m_metadata;
    QStringList m_libraries;
    QString m_extractedPackageDir;
    QString m_appDirPath;
};

#endif // PACKAGETOAPPIMAGEPIPELINE_H





