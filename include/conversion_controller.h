#ifndef CONVERSION_CONTROLLER_H
#define CONVERSION_CONTROLLER_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QThread>
#include <atomic>
#include <mutex>
#include <stop_token>
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

    // Concurrency / cancellation support (SYS-HIGH-11, SYS-HIGH-21)
    std::stop_token stopToken() const;
    void requestStop();

    // Testing helper for pipeline cleanup verification
    PackageToAppImagePipeline* currentPipeline() const;
    void cleanupCurrentPipeline();

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
    bool requiresSudoPassword(const QString& packagePath) const;
    QString appImageOutputPath(const QString& packagePath) const;

    ConversionRequest m_request;
    PackageToAppImagePipeline* m_currentPipeline = nullptr;
    PackageToAppImagePipeline*& m_pipeline = m_currentPipeline;
    QThread* m_pipelineThread = nullptr;
    std::atomic<int> m_currentIndex{0};
    std::atomic<int> m_successCount{0};
    std::atomic<int> m_failureCount{0};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_cancelled{false};
    std::atomic<bool> m_waitingForPassword{false};
    QString m_cachedSudoPassword;
    mutable std::mutex m_stateMutex;
    std::stop_source m_stopSource;
};

#endif // CONVERSION_CONTROLLER_H
