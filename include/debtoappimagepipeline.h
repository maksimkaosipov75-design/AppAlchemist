#ifndef DEBTOAPPIMAGEPIPELINE_H
#define DEBTOAPPIMAGEPIPELINE_H

#include <QObject>
#include <QString>
#include <QThread>
#include "debparser.h"
#include "dependencyanalyzer.h"
#include "appdirbuilder.h"
#include "appimagebuilder.h"

class DebToAppImagePipeline : public QObject {
    Q_OBJECT

public:
    explicit DebToAppImagePipeline(QObject* parent = nullptr);
    ~DebToAppImagePipeline();
    
    void setDebPath(const QString& debPath);
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
    QString m_debPath;
    QString m_outputPath;
    QString m_tempDir;
    bool m_cancelled;
    
    bool validateInput();
    bool extractDeb();
    bool analyzeDependencies();
    bool buildAppDir();
    bool buildAppImage();
    void cleanup();
    
    DebParser* m_parser;
    DependencyAnalyzer* m_analyzer;
    AppDirBuilder* m_appDirBuilder;
    AppImageBuilder* m_appImageBuilder;
    
    DebMetadata m_metadata;
    QStringList m_libraries;
    QString m_extractedDebDir;
    QString m_appDirPath;
};

#endif // DEBTOAPPIMAGEPIPELINE_H






