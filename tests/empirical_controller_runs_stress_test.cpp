#include "conversion_controller.h"
#include "packagetoappimagepipeline.h"
#include "archive_extractor.h"
#include "utils.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTimer>

#include <archive.h>
#include <archive_entry.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
#include <fstream>

static int g_passCount = 0;
static int g_failCount = 0;

#define EMP_TEST(name) \
    std::cout << "\n========================================\n" \
              << "RUNNING TEST: " << name << "\n" \
              << "========================================" << std::endl

#define EMP_ASSERT(cond, msg) \
    do { \
        if (cond) { \
            ++g_passCount; \
            std::cout << "  [PASS] " << msg << std::endl; \
        } else { \
            ++g_failCount; \
            std::cerr << "  [FAIL] " << msg << " (at " << __FILE__ << ":" << __LINE__ << ")" << std::endl; \
        } \
    } while (0)

static long getProcessResidentMemoryKB() {
    long rss = 0;
    std::ifstream statm("/proc/self/statm");
    if (statm >> rss >> rss) {
        long pageSize = sysconf(_SC_PAGESIZE);
        return (rss * pageSize) / 1024;
    }
    return 0;
}

static bool writeTarball(const QString& tarPath) {
    struct archive* a = archive_write_new();
    if (!a) return false;
    archive_write_set_format_pax_restricted(a);
    if (archive_write_open_filename(a, tarPath.toUtf8().constData()) != ARCHIVE_OK) {
        archive_write_free(a);
        return false;
    }

    auto addFile = [&](const char* path, const char* content, mode_t mode) {
        struct archive_entry* entry = archive_entry_new();
        archive_entry_set_pathname(entry, path);
        archive_entry_set_size(entry, std::strlen(content));
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, mode);
        archive_write_header(a, entry);
        archive_write_data(a, content, std::strlen(content));
        archive_entry_free(entry);
    };

    addFile("usr/bin/testapp", "#!/bin/sh\necho ok\n", 0755);
    addFile("usr/share/applications/testapp.desktop",
            "[Desktop Entry]\nType=Application\nName=TestApp\nExec=testapp\nIcon=testapp\n", 0644);
    addFile("usr/share/icons/hicolor/scalable/apps/testapp.svg",
            "<svg width=\"64\" height=\"64\"></svg>", 0644);

    archive_write_close(a);
    archive_write_free(a);
    return true;
}

// ---------------------------------------------------------------------------
// TEST 1: Repeated ConversionController start -> cancel cycles under ASan
// ---------------------------------------------------------------------------
static void test_repeated_controller_cancellation_runs(const QString& tarPath, const QString& outDir) {
    EMP_TEST("ConversionController repeated start -> cancel runs (30 iterations)");

    long memBefore = getProcessResidentMemoryKB();

    for (int i = 0; i < 30; ++i) {
        ConversionController controller;
        ConversionRequest request;
        request.packagePaths = {tarPath, tarPath};
        request.outputDir = outDir;

        QEventLoop loop;
        std::atomic<bool> finishedCalled{false};

        QObject::connect(&controller, &ConversionController::finished, &loop,
            [&](int, int, bool) {
                finishedCalled.store(true);
                loop.quit();
            });

        controller.start(request);
        EMP_ASSERT(controller.isRunning(), QString("Run %1: controller is running").arg(i).toStdString().c_str());

        int delay = (i * 3) % 20; // 0ms to 18ms
        if (delay > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }
        controller.cancel();

        QTimer watchdog;
        watchdog.setSingleShot(true);
        QObject::connect(&watchdog, &QTimer::timeout, &loop, [&]() { loop.quit(); });
        watchdog.start(5000);

        loop.exec();

        EMP_ASSERT(watchdog.isActive(), QString("Run %1: watchdog active (halted promptly)").arg(i).toStdString().c_str());
        EMP_ASSERT(finishedCalled.load(), QString("Run %1: finished signal received").arg(i).toStdString().c_str());
        EMP_ASSERT(!controller.isRunning(), QString("Run %1: controller isRunning is false").arg(i).toStdString().c_str());
        EMP_ASSERT(controller.currentPipeline() == nullptr, QString("Run %1: pipeline was cleaned up").arg(i).toStdString().c_str());
    }

    long memAfter = getProcessResidentMemoryKB();
    long growth = memAfter - memBefore;
    std::cout << "  [INFO] Memory growth after 30 controller start/cancel runs: " << growth << " KB" << std::endl;
}

// ---------------------------------------------------------------------------
// TEST 2: Repeated ConversionController complete/validation runs (20 iterations)
// ---------------------------------------------------------------------------
static void test_repeated_controller_runs_to_completion(const QString& tarPath, const QString& outDir) {
    EMP_TEST("ConversionController repeated conversion runs to completion (20 iterations)");

    long memBefore = getProcessResidentMemoryKB();

    for (int i = 0; i < 20; ++i) {
        ConversionController controller;
        ConversionRequest request;
        request.packagePaths = {tarPath};
        request.outputDir = outDir;

        QEventLoop loop;
        std::atomic<bool> finishedCalled{false};
        int reportedSuccess = 0;
        int reportedFailure = 0;
        bool reportedCancelled = false;

        QObject::connect(&controller, &ConversionController::finished, &loop,
            [&](int succ, int fail, bool cancelled) {
                finishedCalled.store(true);
                reportedSuccess = succ;
                reportedFailure = fail;
                reportedCancelled = cancelled;
                loop.quit();
            });

        controller.start(request);

        QTimer watchdog;
        watchdog.setSingleShot(true);
        QObject::connect(&watchdog, &QTimer::timeout, &loop, [&]() { loop.quit(); });
        watchdog.start(10000);

        loop.exec();

        EMP_ASSERT(watchdog.isActive(), QString("Run %1: completed within timeout").arg(i).toStdString().c_str());
        EMP_ASSERT(finishedCalled.load(), QString("Run %1: finished called").arg(i).toStdString().c_str());
        EMP_ASSERT(!controller.isRunning(), QString("Run %1: not running").arg(i).toStdString().c_str());
        EMP_ASSERT(controller.currentPipeline() == nullptr, QString("Run %1: currentPipeline is null").arg(i).toStdString().c_str());
    }

    long memAfter = getProcessResidentMemoryKB();
    long growth = memAfter - memBefore;
    std::cout << "  [INFO] Memory growth after 20 controller completion runs: " << growth << " KB" << std::endl;
}

// ---------------------------------------------------------------------------
// TEST 3: Multi-package batch queue cancellation midway (10 iterations)
// ---------------------------------------------------------------------------
static void test_repeated_batch_cancellations(const QString& tarPath, const QString& outDir) {
    EMP_TEST("ConversionController 10-package batch cancelled midway (10 iterations)");

    for (int i = 0; i < 10; ++i) {
        ConversionController controller;
        ConversionRequest request;
        // 5 packages per batch
        request.packagePaths = {tarPath, tarPath, tarPath, tarPath, tarPath};
        request.outputDir = outDir;

        QEventLoop loop;
        std::atomic<bool> finishedCalled{false};

        QObject::connect(&controller, &ConversionController::finished, &loop,
            [&](int, int, bool cancelled) {
                finishedCalled.store(true);
                loop.quit();
            });

        controller.start(request);
        EMP_ASSERT(controller.isRunning(), "Controller started batch");

        // Let it start processing first package, then cancel
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        controller.cancel();

        QTimer watchdog;
        watchdog.setSingleShot(true);
        QObject::connect(&watchdog, &QTimer::timeout, &loop, [&]() { loop.quit(); });
        watchdog.start(5000);

        loop.exec();

        EMP_ASSERT(watchdog.isActive(), "Watchdog did not fire");
        EMP_ASSERT(finishedCalled.load(), "Batch finished after cancel");
        EMP_ASSERT(controller.currentIndex() < 5, "Remaining packages skipped in queue");
        EMP_ASSERT(controller.currentPipeline() == nullptr, "Pipeline cleaned up");
    }
}

// ---------------------------------------------------------------------------
// TEST 4: No temporary directories left behind after all runs
// ---------------------------------------------------------------------------
static QStringList listTempWorkDirs() {
    QDir tmpDir(QDir::tempPath());
    return tmpDir.entryList({"appalchemist-*"}, QDir::Dirs | QDir::NoDotAndDotDot);
}

// `preexisting` is captured before the run: any conversion started by another
// process (a real conversion, a parallel CTest job) owns its own temporary
// directory, and counting those would fail this test for unrelated reasons.
static void test_no_stranded_tmp_dirs(const QStringList& preexisting) {
    EMP_TEST("No orphaned /tmp/appalchemist-* directories remaining");
    QStringList remaining = listTempWorkDirs();
    for (const QString& existing : preexisting) {
        remaining.removeAll(existing);
    }
    EMP_ASSERT(remaining.isEmpty(),
               QString("Temporary directory count remaining in /tmp: %1 (expected 0)").arg(remaining.size()).toStdString().c_str());
    if (!remaining.isEmpty()) {
        std::cerr << "  Stranded directories: " << remaining.join(", ").toStdString() << std::endl;
    }
}

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    QTemporaryDir fixtureDir;
    if (!fixtureDir.isValid()) {
        std::cerr << "Failed to create fixture directory" << std::endl;
        return 1;
    }

    const QString tarPath = fixtureDir.filePath("stress-app.tar");
    if (!writeTarball(tarPath)) {
        std::cerr << "Failed to create synthetic tarball" << std::endl;
        return 1;
    }

    const QStringList preexistingTempDirs = listTempWorkDirs();

    test_repeated_controller_cancellation_runs(tarPath, fixtureDir.path());
    test_repeated_controller_runs_to_completion(tarPath, fixtureDir.path());
    test_repeated_batch_cancellations(tarPath, fixtureDir.path());
    test_no_stranded_tmp_dirs(preexistingTempDirs);

    std::cout << "\n========================================" << std::endl;
    std::cout << "SUMMARY: " << g_passCount << " passed, " << g_failCount << " failed." << std::endl;
    std::cout << "========================================" << std::endl;

    return g_failCount == 0 ? 0 : 1;
}
