#include "conversion_controller.h"
#include "repository_browser.h"
#include <QFileInfo>

ConversionController::ConversionController(QObject* parent)
    : QObject(parent)
    , m_pipeline(nullptr)
    , m_pipelineThread(nullptr)
    , m_currentIndex(0)
    , m_successCount(0)
    , m_failureCount(0)
    , m_running(false)
    , m_cancelled(false)
    , m_waitingForPassword(false)
{
}

ConversionController::~ConversionController() {
    cleanupCurrentPipeline();
}

void ConversionController::start(const ConversionRequest& request) {
    if (m_running || request.packagePaths.isEmpty()) {
        return;
    }

    m_request = request;
    m_currentIndex = 0;
    m_successCount = 0;
    m_failureCount = 0;
    m_cancelled = false;
    m_running = true;
    m_waitingForPassword = false;
    m_cachedSudoPassword.clear();

    emit started(m_request.packagePaths.size());
    advanceQueue();
}

void ConversionController::cancel() {
    if (!m_running) {
        return;
    }

    m_cancelled = true;
    m_waitingForPassword = false;

    if (m_pipeline) {
        m_pipeline->cancel();
    } else {
        m_running = false;
        emit finished(m_successCount, m_failureCount, true);
    }
}

void ConversionController::provideSudoPassword(const QString& password) {
    m_cachedSudoPassword = password;
    m_waitingForPassword = false;
    launchCurrentPackage();
}

void ConversionController::continueWithoutSudoPassword() {
    m_waitingForPassword = false;
    launchCurrentPackage();
}

bool ConversionController::isRunning() const {
    return m_running;
}

int ConversionController::currentIndex() const {
    return m_currentIndex;
}

int ConversionController::totalCount() const {
    return m_request.packagePaths.size();
}

int ConversionController::successCount() const {
    return m_successCount;
}

int ConversionController::failureCount() const {
    return m_failureCount;
}

void ConversionController::onPipelineError(const QString& errorMessage) {
    ++m_failureCount;
    emit error(errorMessage);
}

void ConversionController::onPipelineSuccess(const QString& appImagePath) {
    ++m_successCount;
    emit success(appImagePath);
}

void ConversionController::onPipelineFinished() {
    cleanupCurrentPipeline();

    if (m_cancelled) {
        m_running = false;
        emit finished(m_successCount, m_failureCount, true);
        return;
    }

    ++m_currentIndex;
    advanceQueue();
}

void ConversionController::advanceQueue() {
    if (!m_running) {
        return;
    }

    if (m_currentIndex >= m_request.packagePaths.size()) {
        m_running = false;
        emit finished(m_successCount, m_failureCount, false);
        return;
    }

    const QString packagePath = m_request.packagePaths.at(m_currentIndex);
    emit packageStarted(m_currentIndex, m_request.packagePaths.size(), packagePath);

    if (requiresSudoPassword(packagePath) && m_cachedSudoPassword.isEmpty()) {
        m_waitingForPassword = true;
        emit sudoPasswordRequested(
            packagePath,
            tr("Sudo password is required to resolve dependencies via pacman for this package.")
        );
        return;
    }

    launchCurrentPackage();
}

void ConversionController::launchCurrentPackage() {
    if (!m_running || m_waitingForPassword || m_currentIndex >= m_request.packagePaths.size()) {
        return;
    }

    const QString packagePath = m_request.packagePaths.at(m_currentIndex);

    m_pipelineThread = new QThread(this);
    m_pipeline = new PackageToAppImagePipeline();
    m_pipeline->moveToThread(m_pipelineThread);

    connect(m_pipelineThread, &QThread::started, m_pipeline, &PackageToAppImagePipeline::start);
    connect(m_pipeline, &PackageToAppImagePipeline::progress, this, &ConversionController::progress);
    connect(m_pipeline, &PackageToAppImagePipeline::log, this, &ConversionController::log);
    connect(m_pipeline, &PackageToAppImagePipeline::error, this, &ConversionController::onPipelineError);
    connect(m_pipeline, &PackageToAppImagePipeline::success, this, &ConversionController::onPipelineSuccess);
    connect(m_pipeline, &PackageToAppImagePipeline::finished, this, &ConversionController::onPipelineFinished);
    connect(m_pipelineThread, &QThread::finished, m_pipeline, &QObject::deleteLater);

    m_pipeline->setPackagePath(packagePath);
    m_pipeline->setOptimizationSettings(m_request.optimizationSettings);
    m_pipeline->setDependencySettings(m_request.dependencySettings);

    const QString outputPath = appImageOutputPath(packagePath);
    if (!outputPath.isEmpty()) {
        m_pipeline->setOutputPath(outputPath);
    }

    if (!m_cachedSudoPassword.isEmpty()) {
        m_pipeline->setSudoPassword(m_cachedSudoPassword);
    }

    m_pipelineThread->start();
}

void ConversionController::cleanupCurrentPipeline() {
    if (m_pipelineThread) {
        m_pipelineThread->quit();
        m_pipelineThread->wait();
        delete m_pipelineThread;
        m_pipelineThread = nullptr;
    }

    m_pipeline = nullptr;
}

bool ConversionController::requiresSudoPassword(const QString& packagePath) const {
    if (!m_request.dependencySettings.enabled) {
        return false;
    }

    const QFileInfo packageInfo(packagePath);
    const QString suffix = packageInfo.suffix().toLower();

    return suffix == "zst"
        || suffix == "xz"
        || suffix == "gz"
        || packagePath.contains(".pkg.tar")
        || RepositoryBrowser::detectPackageManager() == PackageManager::PACMAN;
}

QString ConversionController::appImageOutputPath(const QString& packagePath) const {
    if (m_request.outputDir.isEmpty()) {
        return QString();
    }

    const QFileInfo packageInfo(packagePath);
    return QString("%1/%2.AppImage")
        .arg(m_request.outputDir, packageInfo.baseName());
}
