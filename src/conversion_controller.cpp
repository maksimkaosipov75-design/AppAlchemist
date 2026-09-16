#include "conversion_controller.h"
#include "repository_browser.h"
#include <QFileInfo>

ConversionController::ConversionController(QObject* parent)
    : QObject(parent)
    , m_currentPipeline(nullptr)
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
    cancel();
    cleanupCurrentPipeline();
}

std::stop_token ConversionController::stopToken() const {
    return m_stopSource.get_token();
}

void ConversionController::requestStop() {
    m_stopSource.request_stop();
    cancel();
}

PackageToAppImagePipeline* ConversionController::currentPipeline() const {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_currentPipeline;
}

void ConversionController::start(const ConversionRequest& request) {
    if (m_running.load() || request.packagePaths.isEmpty()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_request = request;
        m_cachedSudoPassword.clear();
        m_stopSource = std::stop_source();
    }

    m_currentIndex.store(0);
    m_successCount.store(0);
    m_failureCount.store(0);
    m_cancelled.store(false);
    m_running.store(true);
    m_waitingForPassword.store(false);

    emit started(m_request.packagePaths.size());
    advanceQueue();
}

void ConversionController::cancel() {
    m_cancelled.store(true);
    m_waitingForPassword.store(false);
    m_stopSource.request_stop();

    std::lock_guard<std::mutex> lock(m_stateMutex);
    if (m_currentPipeline) {
        m_currentPipeline->cancel();
    }

    if (!m_running.load()) {
        return;
    }

    if (!m_currentPipeline) {
        m_running.store(false);
        emit finished(m_successCount.load(), m_failureCount.load(), true);
    }
}

void ConversionController::provideSudoPassword(const QString& password) {
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_cachedSudoPassword = password;
    }
    m_waitingForPassword.store(false);
    launchCurrentPackage();
}

void ConversionController::continueWithoutSudoPassword() {
    m_waitingForPassword.store(false);
    launchCurrentPackage();
}

bool ConversionController::isRunning() const {
    return m_running.load();
}

int ConversionController::currentIndex() const {
    return m_currentIndex.load();
}

int ConversionController::totalCount() const {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_request.packagePaths.size();
}

int ConversionController::successCount() const {
    return m_successCount.load();
}

int ConversionController::failureCount() const {
    return m_failureCount.load();
}

void ConversionController::onPipelineError(const QString& errorMessage) {
    if (m_cancelled.load()) {
        return;
    }
    ++m_failureCount;
    emit error(errorMessage);
}

void ConversionController::onPipelineSuccess(const QString& appImagePath) {
    if (m_cancelled.load()) {
        return;
    }
    ++m_successCount;
    emit success(appImagePath);
}

void ConversionController::onPipelineFinished() {
    cleanupCurrentPipeline();

    if (m_cancelled.load()) {
        m_running.store(false);
        emit finished(m_successCount.load(), m_failureCount.load(), true);
        return;
    }

    ++m_currentIndex;
    advanceQueue();
}

void ConversionController::advanceQueue() {
    if (!m_running.load()) {
        return;
    }

    if (m_currentIndex.load() >= m_request.packagePaths.size()) {
        m_running.store(false);
        emit finished(m_successCount.load(), m_failureCount.load(), false);
        return;
    }

    const QString packagePath = m_request.packagePaths.at(m_currentIndex.load());
    emit packageStarted(m_currentIndex.load(), m_request.packagePaths.size(), packagePath);

    bool cachedEmpty = false;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        cachedEmpty = m_cachedSudoPassword.isEmpty();
    }

    if (requiresSudoPassword(packagePath) && cachedEmpty) {
        m_waitingForPassword.store(true);
        emit sudoPasswordRequested(
            packagePath,
            tr("Sudo password is required to resolve dependencies via pacman for this package.")
        );
        return;
    }

    launchCurrentPackage();
}

void ConversionController::launchCurrentPackage() {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    if (!m_running.load() || m_waitingForPassword.load() || m_currentIndex.load() >= m_request.packagePaths.size()) {
        return;
    }

    const QString packagePath = m_request.packagePaths.at(m_currentIndex.load());

    m_pipelineThread = new QThread(this);
    m_currentPipeline = new PackageToAppImagePipeline();
    m_currentPipeline->setStopToken(m_stopSource.get_token());
    m_currentPipeline->moveToThread(m_pipelineThread);

    connect(m_pipelineThread, &QThread::started, m_currentPipeline, &PackageToAppImagePipeline::start);
    connect(m_currentPipeline, &PackageToAppImagePipeline::progress, this, &ConversionController::progress);
    connect(m_currentPipeline, &PackageToAppImagePipeline::log, this, &ConversionController::log);
    connect(m_currentPipeline, &PackageToAppImagePipeline::error, this, &ConversionController::onPipelineError);
    connect(m_currentPipeline, &PackageToAppImagePipeline::success, this, &ConversionController::onPipelineSuccess);
    connect(m_currentPipeline, &PackageToAppImagePipeline::finished, this, &ConversionController::onPipelineFinished);

    m_currentPipeline->setPackagePath(packagePath);
    m_currentPipeline->setOptimizationSettings(m_request.optimizationSettings);
    m_currentPipeline->setDependencySettings(m_request.dependencySettings);

    const QString outputPath = appImageOutputPath(packagePath);
    if (!outputPath.isEmpty()) {
        m_currentPipeline->setOutputPath(outputPath);
    }

    if (!m_cachedSudoPassword.isEmpty()) {
        m_currentPipeline->setSudoPassword(m_cachedSudoPassword);
    }

    m_pipelineThread->start();
}

void ConversionController::cleanupCurrentPipeline() {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    if (m_pipelineThread) {
        m_pipelineThread->quit();
        m_pipelineThread->wait();
        delete m_pipelineThread;
        m_pipelineThread = nullptr;
    }

    if (m_currentPipeline) {
        delete m_currentPipeline;
        m_currentPipeline = nullptr;
    }
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
