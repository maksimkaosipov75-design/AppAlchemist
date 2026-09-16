#include "appdirbuilder.h"
#include "appdetector.h"
#include "debparser.h"
#include "packagetoappimagepipeline.h"
#include "conversion_controller.h"
#include "utils.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QString>
#include <QStringList>

#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <archive.h>
#include <archive_entry.h>

static int g_pass = 0;
static int g_fail = 0;

#define EMP_TEST(name) \
    std::cout << "\n=======================================================\n" \
              << "CHALLENGE SUITE: " << name << "\n" \
              << "=======================================================" << std::endl

#define EMP_ASSERT(cond, msg) \
    do { \
        if (cond) { \
            ++g_pass; \
            std::cout << "  [PASS] " << msg << std::endl; \
        } else { \
            ++g_fail; \
            std::cerr << "  [FAIL] " << msg << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; \
        } \
    } while (0)

static QString runBashInEnv(const QString& bashScript, const QProcessEnvironment& env) {
    QProcess proc;
    proc.setProcessEnvironment(env);
    proc.start("bash", {"-c", bashScript});
    if (!proc.waitForFinished(5000)) {
        proc.kill();
        return QString();
    }
    return QString::fromUtf8(proc.readAllStandardOutput());
}

static bool writeSyntheticArchive(const QString& tarPath,
                                  const std::vector<std::tuple<QString, QByteArray, mode_t, QString, QString>>& entries) {
    struct archive* a = archive_write_new();
    archive_write_set_format_pax_restricted(a);
    if (archive_write_open_filename(a, tarPath.toUtf8().constData()) != ARCHIVE_OK) {
        archive_write_free(a);
        return false;
    }
    for (const auto& [name, data, mode, symTarget, hardTarget] : entries) {
        struct archive_entry* e = archive_entry_new();
        archive_entry_set_pathname(e, name.toUtf8().constData());
        archive_entry_set_size(e, data.size());
        archive_entry_set_perm(e, mode & 07777);
        if (!symTarget.isEmpty()) {
            archive_entry_set_filetype(e, AE_IFLNK);
            archive_entry_set_symlink(e, symTarget.toUtf8().constData());
        } else {
            archive_entry_set_filetype(e, AE_IFREG);
        }
        archive_write_header(a, e);
        if (data.size() > 0) {
            archive_write_data(a, data.constData(), data.size());
        }
        archive_entry_free(e);
    }
    archive_write_close(a);
    archive_write_free(a);
    return true;
}

static bool createSyntheticAppTarball(const QString& tarPath) {
    return writeSyntheticArchive(tarPath, {
        {"usr/bin/sample-app", "#!/bin/sh\necho running\n", 0755, "", ""},
        {"usr/share/applications/sample-app.desktop",
         "[Desktop Entry]\nType=Application\nName=SampleApp\nExec=sample-app\nIcon=sample-app\n",
         0644, "", ""},
        {"usr/share/icons/hicolor/scalable/apps/sample-app.svg",
         "<svg width=\"64\" height=\"64\"></svg>",
         0644, "", ""}
    });
}

// ---------------------------------------------------------------------------
// SUITE 1: 100-Iteration Zero-Delay Cancellation Stress & Leaks (SYS-HIGH-11, CON-HIGH-23)
// ---------------------------------------------------------------------------
static void challenge_pipeline_cancellation_stress() {
    EMP_TEST("Pipeline cancellation at delay=0ms (100 iterations) & TempDir leak check");

    QTemporaryDir tempDir;
    EMP_ASSERT(tempDir.isValid(), "Temporary directory created for synthetic packages");
    const QString tarPath = tempDir.filePath("sample-app.tar");
    EMP_ASSERT(createSyntheticAppTarball(tarPath), "Synthetic package created");

    // Helper to count /tmp/appalchemist-* dirs
    auto countTempDirs = []() -> QStringList {
        QDir tmp(QDir::tempPath());
        return tmp.entryList({"appalchemist-*"}, QDir::Dirs | QDir::NoDotAndDotDot);
    };

    QStringList initialTemps = countTempDirs();

    // Test 1A: 50 iterations with delay=0ms using direct cancel()
    bool allCancelledDirect = true;
    bool allPromptDirect = true;
    for (int i = 0; i < 50; ++i) {
        QThread workerThread;
        PackageToAppImagePipeline pipeline;
        pipeline.setPackagePath(tarPath);
        pipeline.setOutputPath(tempDir.filePath(QString("out_%1.AppImage").arg(i)));
        pipeline.moveToThread(&workerThread);

        QEventLoop loop;
        QTimer watchdog;
        watchdog.setSingleShot(true);

        QObject::connect(&workerThread, &QThread::started, &pipeline, &PackageToAppImagePipeline::start);
        QObject::connect(&pipeline, &PackageToAppImagePipeline::finished, &workerThread, &QThread::quit);
        QObject::connect(&pipeline, &PackageToAppImagePipeline::finished, &loop, &QEventLoop::quit);
        QObject::connect(&watchdog, &QTimer::timeout, &loop, &QEventLoop::quit);

        watchdog.start(3000);
        auto t0 = std::chrono::steady_clock::now();
        workerThread.start();

        // 0ms delay: immediately cancel!
        pipeline.cancel();

        loop.exec();
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

        workerThread.quit();
        workerThread.wait(2000);

        if (!pipeline.isCancelled()) {
            allCancelledDirect = false;
            std::cerr << "  [FAIL] Direct cancel iteration " << i << " reported NOT cancelled!" << std::endl;
        }
        if (elapsedMs >= 2000 || !watchdog.isActive()) {
            allPromptDirect = false;
            std::cerr << "  [FAIL] Direct cancel iteration " << i << " timed out or took " << elapsedMs << "ms!" << std::endl;
        }
    }
    EMP_ASSERT(allCancelledDirect, "50 iterations with cancel() delay=0ms were 100% cancelled");
    EMP_ASSERT(allPromptDirect, "50 iterations with cancel() delay=0ms halted promptly (<2000ms)");

    // Test 1B: 50 iterations with delay=0ms using stop_token
    bool allCancelledStopToken = true;
    bool allPromptStopToken = true;
    for (int i = 0; i < 50; ++i) {
        QThread workerThread;
        PackageToAppImagePipeline pipeline;
        std::stop_source stopSource;
        pipeline.setStopToken(stopSource.get_token());
        pipeline.setPackagePath(tarPath);
        pipeline.setOutputPath(tempDir.filePath(QString("out_st_%1.AppImage").arg(i)));
        pipeline.moveToThread(&workerThread);

        QEventLoop loop;
        QTimer watchdog;
        watchdog.setSingleShot(true);

        QObject::connect(&workerThread, &QThread::started, &pipeline, &PackageToAppImagePipeline::start);
        QObject::connect(&pipeline, &PackageToAppImagePipeline::finished, &workerThread, &QThread::quit);
        QObject::connect(&pipeline, &PackageToAppImagePipeline::finished, &loop, &QEventLoop::quit);
        QObject::connect(&watchdog, &QTimer::timeout, &loop, &QEventLoop::quit);

        watchdog.start(3000);
        auto t0 = std::chrono::steady_clock::now();
        workerThread.start();

        // 0ms delay: immediately request stop!
        stopSource.request_stop();

        loop.exec();
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

        workerThread.quit();
        workerThread.wait(2000);

        if (!pipeline.isCancelled()) {
            allCancelledStopToken = false;
            std::cerr << "  [FAIL] stop_token iteration " << i << " reported NOT cancelled!" << std::endl;
        }
        if (elapsedMs >= 2000 || !watchdog.isActive()) {
            allPromptStopToken = false;
            std::cerr << "  [FAIL] stop_token iteration " << i << " timed out or took " << elapsedMs << "ms!" << std::endl;
        }
    }
    EMP_ASSERT(allCancelledStopToken, "50 iterations with stop_token delay=0ms were 100% cancelled");
    EMP_ASSERT(allPromptStopToken, "50 iterations with stop_token delay=0ms halted promptly (<2000ms)");

    // Test 1C: Pre-start cancellation (cancel BEFORE workerThread.start())
    {
        QThread workerThread;
        PackageToAppImagePipeline pipeline;
        pipeline.setPackagePath(tarPath);
        pipeline.setOutputPath(tempDir.filePath("pre_start.AppImage"));
        pipeline.moveToThread(&workerThread);

        // Pre-cancel!
        pipeline.cancel();
        EMP_ASSERT(pipeline.isCancelled(), "Pipeline isCancelled() is true before start()");

        QEventLoop loop;
        QTimer watchdog;
        watchdog.setSingleShot(true);
        QObject::connect(&workerThread, &QThread::started, &pipeline, &PackageToAppImagePipeline::start);
        QObject::connect(&pipeline, &PackageToAppImagePipeline::finished, &workerThread, &QThread::quit);
        QObject::connect(&pipeline, &PackageToAppImagePipeline::finished, &loop, &QEventLoop::quit);
        QObject::connect(&watchdog, &QTimer::timeout, &loop, &QEventLoop::quit);

        watchdog.start(3000);
        workerThread.start();
        loop.exec();

        workerThread.quit();
        workerThread.wait(2000);

        EMP_ASSERT(pipeline.isCancelled(), "Pipeline remains cancelled after pre-start start()");
        EMP_ASSERT(watchdog.isActive(), "Pre-start cancellation completed without timeout");
    }

    // Verify temp directory leak prevention
    QStringList finalTemps = countTempDirs();
    int newlyCreated = 0;
    for (const QString& d : finalTemps) {
        if (!initialTemps.contains(d)) {
            ++newlyCreated;
        }
    }
    EMP_ASSERT(newlyCreated == 0, QString("No temporary directories leaked in /tmp across 100+ cancellation runs (found %1 leaked)").arg(newlyCreated).toStdString().c_str());
}

// ---------------------------------------------------------------------------
// SUITE 2: AppRun Environment Variable Sanitization
// ---------------------------------------------------------------------------
static void challenge_apprun_sanitization() {
    EMP_TEST("AppRun environment variable sanitization across extreme edge cases");

    QTemporaryDir tempDir;
    EMP_ASSERT(tempDir.isValid(), "Temporary directory created");
    const QString appDirPath = tempDir.path();

    // Create directories to trigger all environment exports
    QDir().mkpath(appDirPath + "/usr/bin");
    QDir().mkpath(appDirPath + "/usr/lib");
    QDir().mkpath(appDirPath + "/usr/share/glib-2.0/schemas");
    QDir().mkpath(appDirPath + "/usr/lib/girepository-1.0");
    QDir().mkpath(appDirPath + "/usr/lib/gio/modules");
    QDir().mkpath(appDirPath + "/usr/lib/gtk-3.0/3.0.0/immodules");
    QDir().mkpath(appDirPath + "/usr/lib/gtk-3.0/3.0.0/printbackends");
    QDir().mkpath(appDirPath + "/usr/lib/python3/dist-packages");

    const QString binPath = appDirPath + "/usr/bin/challenge_app";
    QFile binFile(binPath);
    binFile.open(QIODevice::WriteOnly | QIODevice::Text);
    binFile.write("#!/bin/sh\nexit 0\n");
    binFile.close();
    SubprocessWrapper::setExecutable(binPath);

    PackageMetadata meta;
    meta.package = "challenge_app";
    meta.mainExecutable = "usr/bin/challenge_app";
    meta.executables = {"usr/bin/challenge_app"};

    AppDirBuilder builder;
    bool created = builder.createAppRun(appDirPath, meta);
    EMP_ASSERT(created, "createAppRun succeeded");

    const QString appRunPath = appDirPath + "/AppRun";
    QFile f(appRunPath);
    EMP_ASSERT(f.open(QIODevice::ReadOnly | QIODevice::Text), "AppRun opened");
    QString content = QString::fromUtf8(f.readAll());
    f.close();

    // Inspect verbatim text of generated AppRun
    EMP_ASSERT(!content.contains("::"), "AppRun script text contains no double colons");
    EMP_ASSERT(!content.contains(QRegularExpression(R"(:[ \t\r\n"'])")), "AppRun script text contains no colon before whitespace or quotes");
    EMP_ASSERT(!content.contains(QRegularExpression(R"(=":)")), "AppRun script text contains no assignment starting with colon");

    QString evalScript = QString(
        "HERE='%1'; "
        "eval \"$(/bin/sed '/^[[:space:]]*exec /d' '%2')\"; "
        "echo \"LD=$LD_LIBRARY_PATH\"; "
        "echo \"PA=$PATH\"; "
        "echo \"GS=$GSETTINGS_SCHEMA_DIR\"; "
        "echo \"GI=$GI_TYPELIB_PATH\"; "
        "echo \"GT=$GTK_PATH\"; "
        "echo \"XD=$XDG_DATA_DIRS\"; "
        "echo \"XC=$XDG_CONFIG_DIRS\"; "
    ).arg(appDirPath, appRunPath);

    auto checkVars = [](const QString& banner, const QString& output) {
        QStringList lines = output.split('\n', Qt::SkipEmptyParts);
        for (const QString& line : lines) {
            int eq = line.indexOf('=');
            if (eq < 0) continue;
            QString varName = line.left(eq);
            QString val = line.mid(eq + 1);
            EMP_ASSERT(!val.startsWith(":"), QString("[%1] %2 has no leading colon: '%3'").arg(banner, varName, val).toStdString().c_str());
            EMP_ASSERT(!val.endsWith(":"), QString("[%1] %2 has no trailing colon: '%3'").arg(banner, varName, val).toStdString().c_str());
            EMP_ASSERT(!val.contains("::"), QString("[%1] %2 has no double colons: '%3'").arg(banner, varName, val).toStdString().c_str());

            QStringList segments = val.split(':', Qt::KeepEmptyParts);
            EMP_ASSERT(!segments.contains(""), QString("[%1] %2 has no empty segments").arg(banner, varName).toStdString().c_str());
            EMP_ASSERT(!segments.contains("."), QString("[%1] %2 does not contain current directory '.'").arg(banner, varName).toStdString().c_str());
        }
    };

    // Case 1: Completely empty environment
    {
        QProcessEnvironment env;
        checkVars("Clean/Unset Env", runBashInEnv(evalScript, env));
    }

    // Case 2: All variables explicitly empty string ""
    {
        QProcessEnvironment env;
        env.insert("LD_LIBRARY_PATH", "");
        env.insert("PATH", "");
        env.insert("GSETTINGS_SCHEMA_DIR", "");
        env.insert("GI_TYPELIB_PATH", "");
        env.insert("GTK_PATH", "");
        env.insert("XDG_DATA_DIRS", "");
        env.insert("XDG_CONFIG_DIRS", "");
        checkVars("Explicit Empty String Env", runBashInEnv(evalScript, env));
    }

    // Case 3: Host variables set with trailing or double colons
    {
        QProcessEnvironment env;
        env.insert("LD_LIBRARY_PATH", "/host/lib1:/host/lib2");
        env.insert("PATH", "/usr/bin:/bin");
        env.insert("GSETTINGS_SCHEMA_DIR", "/host/schemas");
        env.insert("GI_TYPELIB_PATH", "/host/girepo");
        env.insert("GTK_PATH", "/host/gtk");
        checkVars("Host Populated Env", runBashInEnv(evalScript, env));
    }
}

// ---------------------------------------------------------------------------
// SUITE 3: Single Root .desktop Contract & Icon Synchronization (SYS-HIGH-18)
// ---------------------------------------------------------------------------
static void challenge_single_root_desktop_contract() {
    EMP_TEST("Single root .desktop contract under rogue and multi-desktop conditions");

    QTemporaryDir tempDir;
    EMP_ASSERT(tempDir.isValid(), "Temp directory created");
    const QString appDirPath = tempDir.filePath("AppDir");
    const QString extractedDir = tempDir.filePath("extracted");

    QDir().mkpath(appDirPath);
    QDir().mkpath(extractedDir + "/usr/share/applications");
    QDir().mkpath(extractedDir + "/usr/bin");
    QDir().mkpath(extractedDir + "/usr/share/icons/hicolor/64x64/apps");

    // Plant 6 rogue desktop files in AppDir root
    const QStringList rogues = {
        appDirPath + "/rogue1.desktop",
        appDirPath + "/rogue2.desktop",
        appDirPath + "/attacker.desktop",
        appDirPath + "/z_last.desktop",
        appDirPath + "/0_first.desktop",
        appDirPath + "/canonical.desktop" // same name as canonical to test overwrite
    };
    for (const QString& r : rogues) {
        QFile rf(r);
        rf.open(QIODevice::WriteOnly | QIODevice::Text);
        rf.write("[Desktop Entry]\nName=Rogue\nExec=rogue\n");
        rf.close();
    }

    // Package has 3 desktop files in usr/share/applications
    auto makePkgDesktop = [&](const QString& name, const QString& exec, const QString& icon) {
        QFile df(extractedDir + "/usr/share/applications/" + name);
        df.open(QIODevice::WriteOnly | QIODevice::Text);
        df.write(QString("[Desktop Entry]\nType=Application\nName=%1\nExec=%2\nIcon=%3\nCategories=Utility;\n")
                 .arg(name, exec, icon).toUtf8());
        df.close();
    };
    makePkgDesktop("app-helper.desktop", "helper", "helper");
    makePkgDesktop("canonical.desktop", "main-app", "icon-from-desktop");
    makePkgDesktop("app-settings.desktop", "settings", "settings");

    // Executable
    const QString binPath = extractedDir + "/usr/bin/main-app";
    QFile bf(binPath);
    bf.open(QIODevice::WriteOnly | QIODevice::Text);
    bf.write("#!/bin/sh\nexit 0\n");
    bf.close();
    SubprocessWrapper::setExecutable(binPath);

    // Icon in standard package location
    const QString iconPath = extractedDir + "/usr/share/icons/hicolor/64x64/apps/icon-from-desktop.png";
    QFile icf(iconPath);
    icf.open(QIODevice::WriteOnly);
    icf.write("test-png-data");
    icf.close();

    PackageMetadata meta;
    meta.package = "canonical";
    meta.mainExecutable = binPath;
    meta.executables = {binPath};
    meta.iconPath = ""; // empty: force discovery from desktop + package!

    AppDirBuilder builder;
    bool success = builder.buildAppDir(appDirPath, extractedDir, meta, {});
    EMP_ASSERT(success, "AppDirBuilder::buildAppDir completed successfully");

    // Check AppDir root desktop files
    QStringList rootDesktops = QDir(appDirPath).entryList({"*.desktop"}, QDir::Files);
    EMP_ASSERT(rootDesktops.size() == 1, QString("AppDir root contains EXACTLY 1 desktop file (found %1)").arg(rootDesktops.size()).toStdString().c_str());
    EMP_ASSERT(rootDesktops.first() == "canonical.desktop", "Single root desktop file is canonical.desktop");

    // Verify all 6 planted rogues are gone
    for (const QString& r : rogues) {
        if (QFileInfo(r).fileName() != "canonical.desktop") {
            EMP_ASSERT(!QFile::exists(r), QString("Planted rogue %1 was eradicated").arg(QFileInfo(r).fileName()).toStdString().c_str());
        }
    }

    // Verify Icon= field in root desktop file matches root icon name without extension
    QFile rootDf(appDirPath + "/canonical.desktop");
    EMP_ASSERT(rootDf.open(QIODevice::ReadOnly | QIODevice::Text), "Root desktop file readable");
    QString dfContent = QString::fromUtf8(rootDf.readAll());
    rootDf.close();

    EMP_ASSERT(dfContent.contains("Icon=icon-from-desktop"), "Root desktop Icon= is synchronized to icon-from-desktop");
    EMP_ASSERT(!dfContent.contains("Icon=icon-from-desktop.png"), "Root desktop Icon= does not contain extension .png");
    EMP_ASSERT(!dfContent.contains("/usr/share/icons"), "Root desktop Icon= does not contain absolute path");
}

// ---------------------------------------------------------------------------
// SUITE 4: Relative .DirIcon Symlink Contract (SYS-HIGH-19)
// ---------------------------------------------------------------------------
static void challenge_relative_diricon_symlink() {
    EMP_TEST("Relative .DirIcon symlink contract across all icon sourcing pathways");

    AppDirBuilder builder;

    // Pathway 1: In-package discovery with meta.iconPath = ""
    {
        QTemporaryDir tempDir;
        const QString appDirPath = tempDir.filePath("AppDir");
        const QString extractedDir = tempDir.filePath("extracted");
        QDir().mkpath(appDirPath);
        QDir().mkpath(extractedDir + "/usr/share/applications");
        QDir().mkpath(extractedDir + "/usr/bin");
        QDir().mkpath(extractedDir + "/usr/share/icons/hicolor/48x48/apps");

        QFile df(extractedDir + "/usr/share/applications/pkgapp.desktop");
        df.open(QIODevice::WriteOnly | QIODevice::Text);
        df.write("[Desktop Entry]\nName=PkgApp\nExec=pkgapp\nIcon=pkgapp\n");
        df.close();

        QFile bf(extractedDir + "/usr/bin/pkgapp");
        bf.open(QIODevice::WriteOnly | QIODevice::Text);
        bf.write("#!/bin/sh\nexit 0\n");
        bf.close();
        SubprocessWrapper::setExecutable(bf.fileName());

        QFile ic(extractedDir + "/usr/share/icons/hicolor/48x48/apps/pkgapp.png");
        ic.open(QIODevice::WriteOnly);
        ic.write("genuine-pkg-png");
        ic.close();

        PackageMetadata meta;
        meta.package = "pkgapp";
        meta.mainExecutable = bf.fileName();
        meta.executables = {bf.fileName()};
        meta.iconPath = ""; // empty!

        bool res = builder.buildAppDir(appDirPath, extractedDir, meta, {});
        EMP_ASSERT(res, "[Pathway 1] buildAppDir succeeded with empty meta.iconPath");

        const QString dirIcon = appDirPath + "/.DirIcon";
        EMP_ASSERT(QFile::exists(dirIcon), "[Pathway 1] .DirIcon exists");

        struct stat st;
        EMP_ASSERT(lstat(dirIcon.toUtf8().constData(), &st) == 0 && S_ISLNK(st.st_mode), "[Pathway 1] .DirIcon is POSIX symlink (S_ISLNK)");

        char buf[512] = {0};
        ssize_t len = readlink(dirIcon.toUtf8().constData(), buf, sizeof(buf) - 1);
        EMP_ASSERT(len > 0, "[Pathway 1] readlink succeeded");
        QString target = QString::fromUtf8(buf, len);
        EMP_ASSERT(!target.startsWith("/"), "[Pathway 1] readlink target is relative");
        EMP_ASSERT(target == "pkgapp.png", "[Pathway 1] readlink target is 'pkgapp.png'");
        EMP_ASSERT(QFile::exists(appDirPath + "/" + target), "[Pathway 1] Target file exists in AppDir root");

        QFile targetF(appDirPath + "/" + target);
        targetF.open(QIODevice::ReadOnly);
        QByteArray targetBytes = targetF.readAll();
        targetF.close();
        EMP_ASSERT(targetBytes == "genuine-pkg-png", "[Pathway 1] Root icon contains genuine package icon content");
    }

    // Pathway 2: External meta.iconPath provided
    {
        QTemporaryDir tempDir;
        const QString appDirPath = tempDir.filePath("AppDir");
        const QString extractedDir = tempDir.filePath("extracted");
        QDir().mkpath(appDirPath);
        QDir().mkpath(extractedDir + "/usr/share/applications");
        QDir().mkpath(extractedDir + "/usr/bin");

        QFile df(extractedDir + "/usr/share/applications/extapp.desktop");
        df.open(QIODevice::WriteOnly | QIODevice::Text);
        df.write("[Desktop Entry]\nName=ExtApp\nExec=extapp\nIcon=extapp\n");
        df.close();

        QFile bf(extractedDir + "/usr/bin/extapp");
        bf.open(QIODevice::WriteOnly | QIODevice::Text);
        bf.write("#!/bin/sh\nexit 0\n");
        bf.close();
        SubprocessWrapper::setExecutable(bf.fileName());

        const QString extIconPath = tempDir.filePath("external_logo.png");
        QFile extIc(extIconPath);
        extIc.open(QIODevice::WriteOnly);
        extIc.write("genuine-external-icon-content");
        extIc.close();

        PackageMetadata meta;
        meta.package = "extapp";
        meta.mainExecutable = bf.fileName();
        meta.executables = {bf.fileName()};
        meta.iconPath = extIconPath; // external!

        bool res = builder.buildAppDir(appDirPath, extractedDir, meta, {});
        EMP_ASSERT(res, "[Pathway 2] buildAppDir succeeded with external meta.iconPath");

        const QString dirIcon = appDirPath + "/.DirIcon";
        EMP_ASSERT(QFile::exists(dirIcon), "[Pathway 2] .DirIcon exists");

        struct stat st;
        EMP_ASSERT(lstat(dirIcon.toUtf8().constData(), &st) == 0 && S_ISLNK(st.st_mode), "[Pathway 2] .DirIcon is POSIX symlink (S_ISLNK)");

        char buf[512] = {0};
        ssize_t len = readlink(dirIcon.toUtf8().constData(), buf, sizeof(buf) - 1);
        EMP_ASSERT(len > 0, "[Pathway 2] readlink succeeded");
        QString target = QString::fromUtf8(buf, len);
        EMP_ASSERT(!target.startsWith("/"), "[Pathway 2] readlink target is relative");
        EMP_ASSERT(target == "extapp.png", "[Pathway 2] readlink target is 'extapp.png'");
        EMP_ASSERT(QFile::exists(appDirPath + "/" + target), "[Pathway 2] Target file exists in AppDir root");

        QFile targetF(appDirPath + "/" + target);
        targetF.open(QIODevice::ReadOnly);
        QByteArray targetBytes = targetF.readAll();
        targetF.close();
        EMP_ASSERT(targetBytes == "genuine-external-icon-content", "[Pathway 2] External icon preserved and NOT overwritten by placeholder");
    }

    // Pathway 3: Fallback 1x1 placeholder when NO icon exists anywhere
    {
        QTemporaryDir tempDir;
        const QString appDirPath = tempDir.filePath("AppDir");
        const QString extractedDir = tempDir.filePath("extracted");
        QDir().mkpath(appDirPath);
        QDir().mkpath(extractedDir + "/usr/share/applications");
        QDir().mkpath(extractedDir + "/usr/bin");

        QFile df(extractedDir + "/usr/share/applications/noiconapp.desktop");
        df.open(QIODevice::WriteOnly | QIODevice::Text);
        df.write("[Desktop Entry]\nName=NoIconApp\nExec=noiconapp\nIcon=noiconapp\n");
        df.close();

        QFile bf(extractedDir + "/usr/bin/noiconapp");
        bf.open(QIODevice::WriteOnly | QIODevice::Text);
        bf.write("#!/bin/sh\nexit 0\n");
        bf.close();
        SubprocessWrapper::setExecutable(bf.fileName());

        PackageMetadata meta;
        meta.package = "noiconapp";
        meta.mainExecutable = bf.fileName();
        meta.executables = {bf.fileName()};
        meta.iconPath = ""; // empty! and no icon in package!

        bool res = builder.buildAppDir(appDirPath, extractedDir, meta, {});
        EMP_ASSERT(res, "[Pathway 3] buildAppDir succeeded with fallback placeholder");

        const QString dirIcon = appDirPath + "/.DirIcon";
        EMP_ASSERT(QFile::exists(dirIcon), "[Pathway 3] .DirIcon exists");

        struct stat st;
        EMP_ASSERT(lstat(dirIcon.toUtf8().constData(), &st) == 0 && S_ISLNK(st.st_mode), "[Pathway 3] .DirIcon is POSIX symlink (S_ISLNK)");

        char buf[512] = {0};
        ssize_t len = readlink(dirIcon.toUtf8().constData(), buf, sizeof(buf) - 1);
        EMP_ASSERT(len > 0, "[Pathway 3] readlink succeeded");
        QString target = QString::fromUtf8(buf, len);
        EMP_ASSERT(!target.startsWith("/"), "[Pathway 3] readlink target is relative");
        EMP_ASSERT(target == "noiconapp.png", "[Pathway 3] readlink target is 'noiconapp.png'");
        EMP_ASSERT(QFile::exists(appDirPath + "/" + target), "[Pathway 3] Placeholder icon file exists in AppDir root");
    }
}

// ---------------------------------------------------------------------------
// SUITE 5: ConversionController Lifecycle & Active Cancellation (MEM-MED-25)
// ---------------------------------------------------------------------------
static void challenge_conversion_controller_lifecycle() {
    EMP_TEST("ConversionController lifecycle destruction while active");

    QTemporaryDir tempDir;
    const QString tarPath = tempDir.filePath("ctrl-app.tar");
    EMP_ASSERT(createSyntheticAppTarball(tarPath), "Synthetic package created for controller test");

    // Destruct controller while active pipeline is in flight
    for (int i = 0; i < 10; ++i) {
        auto* controller = new ConversionController();
        ConversionRequest req;
        req.packagePaths = {tarPath};
        req.outputDir = tempDir.path();
        controller->start(req);

        // Wait a tiny bit (0ms to 10ms) so the thread actually starts
        if (i % 2 == 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        // Deleting controller must cancel pipeline and cleanly wait for thread without deadlock or leak
        delete controller;
    }
    EMP_ASSERT(true, "10 iterations of active ConversionController destruction completed without hang or crash");
}

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    std::cout << "=======================================================\n"
              << "APPALCHEMIST EMPIRICAL CHALLENGER FRESH STRESS HARNESS\n"
              << "=======================================================" << std::endl;

    challenge_pipeline_cancellation_stress();
    challenge_apprun_sanitization();
    challenge_single_root_desktop_contract();
    challenge_relative_diricon_symlink();
    challenge_conversion_controller_lifecycle();

    std::cout << "\n=======================================================\n"
              << "FINAL CHALLENGER RESULTS: " << g_pass << " passed, " << g_fail << " failed.\n"
              << "=======================================================" << std::endl;

    return (g_fail == 0) ? 0 : 1;
}
