#include <catch2/catch_test_macros.hpp>
#include "appdirbuilder.h"
#include "dependency_resolver.h"
#include "dependencyanalyzer.h"
#include "debparser.h"
#include "rpmparser.h"
#include "test_helpers.h"
#include "utils.h"
#include <QTemporaryDir>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QProcess>
#include <QProcessEnvironment>

TEST_CASE("AppDirBuilder: AppRun template generation and syntax check", "[appdir][apprun]") {
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());
    const QString appDirPath = tempDir.path();

    REQUIRE(QDir().mkpath(appDirPath + "/usr/bin"));
    REQUIRE(QDir().mkpath(appDirPath + "/usr/lib"));

    const QString dummyBin = appDirPath + "/usr/bin/sample-app";
    REQUIRE(TestHelpers::createSampleElf(dummyBin));

    PackageMetadata meta;
    meta.package = "sample-app";
    meta.mainExecutable = "usr/bin/sample-app";
    meta.executables = {"usr/bin/sample-app"};

    AppDirBuilder builder;
    REQUIRE(builder.createAppRun(appDirPath, meta));

    const QString appRunPath = appDirPath + "/AppRun";
    REQUIRE(QFile::exists(appRunPath));

    // 1. Permissions check
    QFileInfo appRunInfo(appRunPath);
    REQUIRE((appRunInfo.permissions() & QFile::ExeOwner) != 0);

    // 2. Syntax check via /bin/bash -n
    QProcess syntaxCheck;
    syntaxCheck.start("bash", {"-n", appRunPath});
    REQUIRE(syntaxCheck.waitForFinished(3000));
    REQUIRE(syntaxCheck.exitCode() == 0);

    // 3. Trailing colons check
    QFile f(appRunPath);
    REQUIRE(f.open(QIODevice::ReadOnly | QIODevice::Text));
    QString appRunContent = f.readAll();
    f.close();

    REQUIRE_FALSE(appRunContent.contains(":${LD_LIBRARY_PATH}\""));
    REQUIRE_FALSE(appRunContent.contains(":${GSETTINGS_SCHEMA_DIR}\""));
    REQUIRE_FALSE(appRunContent.contains(":${GI_TYPELIB_PATH}\""));
    REQUIRE(appRunContent.contains("${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"));
}

TEST_CASE("AppRun exposes Qt plugin and QML paths shipped inside the package",
          "[appdir][apprun][qt]") {
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());
    const QString appDirPath = tempDir.path();

    REQUIRE(QDir().mkpath(appDirPath + "/usr/bin"));
    REQUIRE(QDir().mkpath(appDirPath + "/usr/lib"));
    // A Qt application that ships its own runtime keeps the platform plugins
    // and QML modules next to it instead of in usr/lib.
    REQUIRE(QDir().mkpath(appDirPath + "/opt/sample-app/lib/plugins/platforms"));
    REQUIRE(QDir().mkpath(appDirPath + "/opt/sample-app/lib/qml/QtQuick/Controls"));

    const QString dummyBin = appDirPath + "/usr/bin/sample-app";
    REQUIRE(TestHelpers::createSampleElf(dummyBin));
    REQUIRE(TestHelpers::createSampleElf(appDirPath + "/opt/sample-app/lib/plugins/platforms/libqxcb.so"));

    PackageMetadata meta;
    meta.package = "sample-app";
    meta.mainExecutable = "usr/bin/sample-app";
    meta.executables = {"usr/bin/sample-app"};

    AppDirBuilder builder;
    REQUIRE(builder.createAppRun(appDirPath, meta));

    QFile appRun(appDirPath + "/AppRun");
    REQUIRE(appRun.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString content = appRun.readAll();
    appRun.close();

    SECTION("platform plugins are reachable") {
        REQUIRE(content.contains("QT_PLUGIN_PATH"));
        REQUIRE(content.contains("opt/sample-app/lib/plugins"));
        REQUIRE(content.contains("QT_QPA_PLATFORM_PLUGIN_PATH"));
    }

    SECTION("QML modules are reachable") {
        REQUIRE(content.contains("QML2_IMPORT_PATH"));
        REQUIRE(content.contains("QML_IMPORT_PATH"));
        REQUIRE(content.contains("opt/sample-app/lib/qml"));
    }

    SECTION("existing values are preserved without leading colons") {
        REQUIRE(content.contains("${QT_PLUGIN_PATH:+:${QT_PLUGIN_PATH}}"));
        REQUIRE(content.contains("${QML2_IMPORT_PATH:+:${QML2_IMPORT_PATH}}"));
        REQUIRE_FALSE(content.contains(":${QT_PLUGIN_PATH}\""));
    }

    SECTION("the generated script still parses") {
        QProcess syntaxCheck;
        syntaxCheck.start("bash", {"-n", appDirPath + "/AppRun"});
        REQUIRE(syntaxCheck.waitForFinished(3000));
        REQUIRE(syntaxCheck.exitCode() == 0);
    }
}

TEST_CASE("Library bundling prefers package-provided libraries over host copies",
          "[deps][bundling]") {
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());
    const QString appDirPath = tempDir.path();

    REQUIRE(QDir().mkpath(appDirPath + "/usr/bin"));
    REQUIRE(QDir().mkpath(appDirPath + "/usr/lib"));
    REQUIRE(QDir().mkpath(appDirPath + "/opt/sample-app/lib"));

    REQUIRE(TestHelpers::createSampleElf(appDirPath + "/usr/bin/sample-app"));
    // The package ships its own libsample.so.1 ...
    REQUIRE(TestHelpers::createSampleElf(appDirPath + "/opt/sample-app/lib/libsample.so.1"));
    // ... while an earlier staging step left a host copy and a soname symlink
    // whose target was never copied in.
    REQUIRE(TestHelpers::createSampleElf(appDirPath + "/usr/lib/libsample.so.1"));
    REQUIRE(QFile::link("libdangling.so.2.1", appDirPath + "/usr/lib/libdangling.so.2"));

    DependencyResolver resolver;
    const LibraryBundleReport report = resolver.bundleSystemLibraries(appDirPath);
    REQUIRE(report.ran);

    SECTION("the host copy no longer shadows the package's own library") {
        REQUIRE_FALSE(QFileInfo::exists(appDirPath + "/usr/lib/libsample.so.1"));
        REQUIRE(QFileInfo::exists(appDirPath + "/opt/sample-app/lib/libsample.so.1"));
    }

    SECTION("symlinks that resolve to nothing are not shipped") {
        const QFileInfo dangling(appDirPath + "/usr/lib/libdangling.so.2");
        REQUIRE_FALSE((dangling.isSymLink() && !dangling.exists()));
    }
}

TEST_CASE("Cross-Feature: RPM dependency extraction into AppDir usr/lib", "[rpm][deps][appdir]") {
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());

    // Setup staged dependencies representing extracted RPM requires
    const QString stagedDeps = tempDir.path() + "/staged_deps/usr/lib";
    REQUIRE(QDir().mkpath(stagedDeps));

    const QString realLib = stagedDeps + "/libsample-rpm.so.1.2.3";
    QFile f(realLib);
    REQUIRE(f.open(QIODevice::WriteOnly));
    f.write("SAMPLE_RPM_ELF_LIBRARY_BYTES");
    f.close();

    // Create soname symlinks
    const QString sonameMajor = stagedDeps + "/libsample-rpm.so.1";
    const QString sonameBase = stagedDeps + "/libsample-rpm.so";
    REQUIRE(QFile::link("libsample-rpm.so.1.2.3", sonameMajor));
    REQUIRE(QFile::link("libsample-rpm.so.1", sonameBase));

    // Target AppDir usr/lib
    const QString appDirPath = tempDir.path() + "/AppDir";
    const QString targetLibDir = appDirPath + "/usr/lib";
    REQUIRE(QDir().mkpath(targetLibDir));

    // Copy staged dependencies
    REQUIRE(SubprocessWrapper::copyDirectory(stagedDeps, targetLibDir));

    // Assert files and symlinks are preserved
    REQUIRE(QFile::exists(targetLibDir + "/libsample-rpm.so.1.2.3"));

    QFileInfo symMajorInfo(targetLibDir + "/libsample-rpm.so.1");
    REQUIRE(symMajorInfo.isSymLink());
    REQUIRE(QFileInfo(symMajorInfo.symLinkTarget()).fileName() == "libsample-rpm.so.1.2.3");

    QFileInfo symBaseInfo(targetLibDir + "/libsample-rpm.so");
    REQUIRE(symBaseInfo.isSymLink());
    REQUIRE(QFileInfo(symBaseInfo.symLinkTarget()).fileName() == "libsample-rpm.so.1");
}

TEST_CASE("Cross-Feature: Pipeline passive readelf DT_NEEDED fallback on bubblewrap bypass", "[pipeline][readelf][fallback]") {
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());

    QString hostBinary = "/bin/echo";
    if (!QFile::exists(hostBinary)) {
        hostBinary = "/usr/bin/echo";
    }
    if (!QFile::exists(hostBinary)) {
        hostBinary = "/bin/ls";
    }
    REQUIRE(QFile::exists(hostBinary));

    const QString tempBinary = tempDir.filePath("sample_elf_target");
    REQUIRE(QFile::copy(hostBinary, tempBinary));
    REQUIRE(SubprocessWrapper::setExecutable(tempBinary));

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("APPALCHEMIST_DISABLE_BWRAP", "1");

    QStringList resolverOutput = DependencyResolver::runSafeLdd(tempBinary, env);
    REQUIRE_FALSE(resolverOutput.isEmpty());

    bool foundLibc = false;
    for (const QString& line : resolverOutput) {
        if (line.contains("libc.so")) {
            foundLibc = true;
            break;
        }
    }
    REQUIRE(foundLibc);

    QStringList analyzerOutput = DependencyAnalyzer::runLdd(tempBinary, env);
    REQUIRE_FALSE(analyzerOutput.isEmpty());
    bool analyzerFoundLibc = false;
    for (const QString& line : analyzerOutput) {
        if (line.contains("libc.so")) {
            analyzerFoundLibc = true;
            break;
        }
    }
    REQUIRE(analyzerFoundLibc);
}

TEST_CASE("Workload: Complete synthetic package to AppDir synthesis", "[e2e][workload]") {
    QTemporaryDir tempDir;
    REQUIRE(tempDir.isValid());

    // 1. Build a synthetic .deb package
    QString debPath = tempDir.filePath("full-app_1.0.0_amd64.deb");
    REQUIRE(TestHelpers::buildSyntheticDeb(debPath, "full-app", "1.0.0", "Full E2E Synthetic Application"));

    // 2. Extract deb package using DebParser
    DebParser parser;
    QString extractDir = tempDir.filePath("extracted");
    REQUIRE(parser.extractDeb(debPath, extractDir));

    // 3. Extract metadata using public parseMetadata()
    PackageMetadata meta = parser.parseMetadata(extractDir);
    REQUIRE_FALSE(meta.executables.isEmpty());
    REQUIRE(meta.package == "full-app");

    // 4. Synthesize complete AppDir
    QString appDirPath = tempDir.filePath("full-app.AppDir");
    AppDirBuilder builder;
    REQUIRE(builder.buildAppDir(appDirPath, extractDir, meta, {}));

    // 5. Assertions:
    // Exactly one root desktop file
    QDir appDirObj(appDirPath);
    QStringList desktopEntries = appDirObj.entryList({"*.desktop"}, QDir::Files);
    REQUIRE(desktopEntries.size() == 1);
    QString rootDesktop = appDirObj.filePath(desktopEntries.first());

    // Exec=AppRun in root desktop
    QFile deskFile(rootDesktop);
    REQUIRE(deskFile.open(QIODevice::ReadOnly | QIODevice::Text));
    QString deskContent = deskFile.readAll();
    deskFile.close();
    REQUIRE(deskContent.contains("Exec=AppRun"));

    // .DirIcon relative symlink exists in AppDir root
    QString dirIconPath = appDirPath + "/.DirIcon";
    QFileInfo dirIconInfo(dirIconPath);
    REQUIRE(dirIconInfo.exists());
    REQUIRE(dirIconInfo.isSymLink());

    // AppRun exists and is executable
    QString appRunPath = appDirPath + "/AppRun";
    QFileInfo appRunInfo(appRunPath);
    REQUIRE(appRunInfo.exists());
    REQUIRE((appRunInfo.permissions() & QFile::ExeOwner) != 0);

    // Executable exists in usr/bin
    QString binPath = appDirPath + "/usr/bin/full-app";
    REQUIRE(QFile::exists(binPath));
}
