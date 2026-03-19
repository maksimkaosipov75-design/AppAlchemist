#ifndef CONVERSION_CONTROLLER_H
#define CONVERSION_CONTROLLER_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QThread>
#include "packagetoappimagepipeline.h"
#include "dependency_resolver.h"
#include "size_optimizer.h"

struct ConversionRequest {
    QStringList packagePaths;
    QString outputDir;
    OptimizationSettings optimizationSettings;
    DependencySettings dependencySettings;
};

class ConversionController : public QObject {
    Q_OBJECT

public:
    explicit ConversionController(QObject* parent = nullptr);
    ~ConversionController();

    void start(const ConversionRequest& request);
    void cancel();
    void provideSudoPassword(const QString& password);
    void continueWithoutSudoPassword();

    bool isRunning() const;
    int currentIndex() const;
    int totalCount() const;
    int successCount() const;
    int failureCount() const;

signals:
    void started(int totalCount);
    void packageStarted(int index, int totalCount, const QString& packagePath);
    void progress(int percentage, const QString& message);
    void log(const QString& message);
    void error(const QString& errorMessage);
    void success(const QString& appImagePath);
    void sudoPasswordRequested(const QString& packagePath, const QString& reason);
    void finished(int successCount, int failureCount, bool cancelled);

private slots:
    void onPipelineError(const QString& errorMessage);
    void onPipelineSuccess(const QString& appImagePath);
    void onPipelineFinished();

private:
    void advanceQueue();
    void launchCurrentPackage();
    void cleanupCurrentPipeline();
    bool requiresSudoPassword(const QString& packagePath) const;
    QString appImageOutputPath(const QString& packagePath) const;

    ConversionRequest m_request;
    PackageToAppImagePipeline* m_pipeline;
    QThread* m_pipelineThread;
    int m_currentIndex;
    int m_successCount;
    int m_failureCount;
    bool m_running;
    bool m_cancelled;
    bool m_waitingForPassword;
    QString m_cachedSudoPassword;
};

#endif // CONVERSION_CONTROLLER_H
