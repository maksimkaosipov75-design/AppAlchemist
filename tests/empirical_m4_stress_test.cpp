#include "appdirbuilder.h"
#include "appdetector.h"
#include "debparser.h"
#include "utils.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QString>
#include <QStringList>

#include <iostream>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

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

// Helper: Run bash command in a clean/specified environment and capture stdout
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

// ---------------------------------------------------------------------------
// TEST SUITE 1: AppRun Environment Variable Generation & Bash Execution
// ---------------------------------------------------------------------------
static void test_apprun_environment_variables() {
    EMP_TEST("AppRun environment variable generation & bash execution");

    QTemporaryDir tempDir;
    EMP_ASSERT(tempDir.isValid(), "Temporary directory created");
    const QString appDirPath = tempDir.path();

    // Setup full directory structure to trigger all exports
    QDir().mkpath(appDirPath + "/usr/bin");
    QDir().mkpath(appDirPath + "/usr/lib");
    QDir().mkpath(appDirPath + "/usr/share/glib-2.0/schemas");
    QDir().mkpath(appDirPath + "/usr/lib/girepository-1.0");
    QDir().mkpath(appDirPath + "/usr/lib/gio/modules");
    QDir().mkpath(appDirPath + "/usr/lib/gtk-3.0/3.0.0/immodules");
    QDir().mkpath(appDirPath + "/usr/lib/gtk-3.0/3.0.0/printbackends");

    // Dummy executable
    const QString binPath = appDirPath + "/usr/bin/emp_app";
    QFile binFile(binPath);
    binFile.open(QIODevice::WriteOnly | QIODevice::Text);
    binFile.write("#!/bin/sh\nexit 0\n");
    binFile.close();
    SubprocessWrapper::setExecutable(binPath);

    PackageMetadata meta;
    meta.package = "emp_app";
    meta.mainExecutable = "usr/bin/emp_app";
    meta.executables = {"usr/bin/emp_app"};

    AppDirBuilder builder;
    bool created = builder.createAppRun(appDirPath, meta);
    EMP_ASSERT(created, "AppDirBuilder::createAppRun returned true");

    const QString appRunPath = appDirPath + "/AppRun";
    EMP_ASSERT(QFile::exists(appRunPath), "AppRun exists at AppDir root");

    QFile appRunFile(appRunPath);
    EMP_ASSERT(appRunFile.open(QIODevice::ReadOnly | QIODevice::Text), "AppRun readable");
    QString appRunContent = QString::fromUtf8(appRunFile.readAll());
    appRunFile.close();

    // Regex checks on generated AppRun script itself:
    // Regex 1: colon followed by whitespace, quote, or newline: `:[ \t\r\n"']`
    QRegularExpression trailingColonRegex(R"(:[ \t\r\n"'])");
    QRegularExpressionMatch matchTrailing = trailingColonRegex.match(appRunContent);
    EMP_ASSERT(!matchTrailing.hasMatch(), 
               QString("AppRun script must not contain trailing colons or colons before quotes/whitespace (matched: '%1')")
               .arg(matchTrailing.hasMatch() ? matchTrailing.captured(0) : "none").toStdString().c_str());

    // Regex 2: assignment starting with colon: `=":`
    QRegularExpression leadingColonRegex(R"(=":)");
    QRegularExpressionMatch matchLeading = leadingColonRegex.match(appRunContent);
    EMP_ASSERT(!matchLeading.hasMatch(),
               QString("AppRun script must not contain leading colons in assignments `=\":` (matched: '%1')")
               .arg(matchLeading.hasMatch() ? matchLeading.captured(0) : "none").toStdString().c_str());

    // Regex 3: verify presence of safe parameter expansions
    EMP_ASSERT(appRunContent.contains("${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"),
               "AppRun contains safe ${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}");
    EMP_ASSERT(appRunContent.contains("${GSETTINGS_SCHEMA_DIR:+:${GSETTINGS_SCHEMA_DIR}}"),
               "AppRun contains safe ${GSETTINGS_SCHEMA_DIR:+:${GSETTINGS_SCHEMA_DIR}}");
    EMP_ASSERT(appRunContent.contains("${GI_TYPELIB_PATH:+:${GI_TYPELIB_PATH}}"),
               "AppRun contains safe ${GI_TYPELIB_PATH:+:${GI_TYPELIB_PATH}}");

    // Execution under Bash:
    // We source the AppRun environment (stripping 'exec') and print variable values
    QString evalScript = QString(
        "HERE='%1'; "
        "eval \"$(sed '/^[[:space:]]*exec /d' '%2')\"; "
        "echo \"LD=$LD_LIBRARY_PATH\"; "
        "echo \"GS=$GSETTINGS_SCHEMA_DIR\"; "
        "echo \"GI=$GI_TYPELIB_PATH\"; "
        "echo \"GTK=$GTK_PATH\"; "
    ).arg(appDirPath, appRunPath);

    // Scenario A: Completely clean environment (all variables unset)
    {
        QProcessEnvironment cleanEnv; // empty
        QString output = runBashInEnv(evalScript, cleanEnv);

        QString ldVal, gsVal, giVal, gtkVal;
        for (const QString& line : output.split('\n')) {
            if (line.startsWith("LD=")) ldVal = line.mid(3);
            else if (line.startsWith("GS=")) gsVal = line.mid(3);
            else if (line.startsWith("GI=")) giVal = line.mid(3);
            else if (line.startsWith("GTK=")) gtkVal = line.mid(4);
        }

        EMP_ASSERT(!ldVal.isEmpty(), "LD_LIBRARY_PATH evaluated to non-empty string in clean env");
        EMP_ASSERT(!ldVal.startsWith(":"), "LD_LIBRARY_PATH has no leading colon");
        EMP_ASSERT(!ldVal.endsWith(":"), "LD_LIBRARY_PATH has no trailing colon");
        EMP_ASSERT(!ldVal.contains("::"), "LD_LIBRARY_PATH has no double colons");

        // Verify CWD '.' is not in LD_LIBRARY_PATH
        QStringList ldParts = ldVal.split(':');
        bool containsCwd = ldParts.contains(".") || ldParts.contains("");
        EMP_ASSERT(!containsCwd, "LD_LIBRARY_PATH does not include CWD '.' or empty element");

        // Verify GSETTINGS_SCHEMA_DIR
        EMP_ASSERT(!gsVal.isEmpty(), "GSETTINGS_SCHEMA_DIR evaluated to non-empty string");
        EMP_ASSERT(!gsVal.startsWith(":") && !gsVal.endsWith(":") && !gsVal.contains("::"),
                   "GSETTINGS_SCHEMA_DIR has no leading/trailing/double colons");
        EMP_ASSERT(!gsVal.split(':').contains(".") && !gsVal.split(':').contains(""),
                   "GSETTINGS_SCHEMA_DIR does not include CWD '.'");

        // Verify GI_TYPELIB_PATH
        EMP_ASSERT(!giVal.isEmpty(), "GI_TYPELIB_PATH evaluated to non-empty string");
        EMP_ASSERT(!giVal.startsWith(":") && !giVal.endsWith(":") && !giVal.contains("::"),
                   "GI_TYPELIB_PATH has no leading/trailing/double colons");
        EMP_ASSERT(!giVal.split(':').contains(".") && !giVal.split(':').contains(""),
                   "GI_TYPELIB_PATH does not include CWD '.'");

        // Verify GTK_PATH
        EMP_ASSERT(!gtkVal.isEmpty(), "GTK_PATH evaluated to non-empty string");
        EMP_ASSERT(!gtkVal.startsWith(":") && !gtkVal.endsWith(":") && !gtkVal.contains("::"),
                   "GTK_PATH has no leading/trailing/double colons");
    }

    // Scenario B: Host variables explicitly set to empty string
    {
        QProcessEnvironment emptyVarsEnv;
        emptyVarsEnv.insert("LD_LIBRARY_PATH", "");
        emptyVarsEnv.insert("GSETTINGS_SCHEMA_DIR", "");
        emptyVarsEnv.insert("GI_TYPELIB_PATH", "");
        emptyVarsEnv.insert("GTK_PATH", "");
        emptyVarsEnv.insert("PYTHONPATH", "");

        QString output = runBashInEnv(evalScript, emptyVarsEnv);

        QString ldVal, gsVal, giVal;
        for (const QString& line : output.split('\n')) {
            if (line.startsWith("LD=")) ldVal = line.mid(3);
            else if (line.startsWith("GS=")) gsVal = line.mid(3);
            else if (line.startsWith("GI=")) giVal = line.mid(3);
        }

        EMP_ASSERT(!ldVal.endsWith(":"), "Empty host LD_LIBRARY_PATH does not produce trailing colon");
        EMP_ASSERT(!ldVal.startsWith(":"), "Empty host LD_LIBRARY_PATH does not produce leading colon");
        EMP_ASSERT(!ldVal.contains("::"), "Empty host LD_LIBRARY_PATH does not produce double colon");
        EMP_ASSERT(!ldVal.split(':').contains(".") && !ldVal.split(':').contains(""),
                   "Empty host LD_LIBRARY_PATH does not include CWD '.'");

        EMP_ASSERT(!gsVal.endsWith(":") && !gsVal.startsWith(":") && !gsVal.contains("::"),
                   "Empty host GSETTINGS_SCHEMA_DIR does not produce leading/trailing/double colon");
        EMP_ASSERT(!giVal.endsWith(":") && !giVal.startsWith(":") && !giVal.contains("::"),
                   "Empty host GI_TYPELIB_PATH does not produce leading/trailing/double colon");
    }

    // Scenario C: Host variables set with existing paths
    {
        QProcessEnvironment populatedEnv;
        populatedEnv.insert("LD_LIBRARY_PATH", "/host/custom/lib");
        populatedEnv.insert("GSETTINGS_SCHEMA_DIR", "/host/glib/schemas");
        populatedEnv.insert("GI_TYPELIB_PATH", "/host/girepository");

        QString output = runBashInEnv(evalScript, populatedEnv);

        QString ldVal, gsVal, giVal;
        for (const QString& line : output.split('\n')) {
            if (line.startsWith("LD=")) ldVal = line.mid(3);
            else if (line.startsWith("GS=")) gsVal = line.mid(3);
            else if (line.startsWith("GI=")) giVal = line.mid(3);
        }

        EMP_ASSERT(ldVal.endsWith(":/host/custom/lib"), "Host LD_LIBRARY_PATH appended at end with colon separator");
        EMP_ASSERT(!ldVal.startsWith(":") && !ldVal.contains("::"), "No leading or double colons with populated LD_LIBRARY_PATH");
        EMP_ASSERT(gsVal.endsWith(":/host/glib/schemas"), "Host GSETTINGS_SCHEMA_DIR appended at end");
        EMP_ASSERT(giVal.endsWith(":/host/girepository"), "Host GI_TYPELIB_PATH appended at end");
    }

    // Python Application AppRun Test
    {
        QTemporaryDir pyTempDir;
        const QString pyAppDirPath = pyTempDir.path();
        QDir().mkpath(pyAppDirPath + "/usr/bin");
        QDir().mkpath(pyAppDirPath + "/usr/lib/python3/dist-packages");

        const QString pyBin = pyAppDirPath + "/usr/bin/pyscript.py";
        QFile pyBinFile(pyBin);
        pyBinFile.open(QIODevice::WriteOnly | QIODevice::Text);
        pyBinFile.write("#!/usr/bin/env python3\nprint('hello')\n");
        pyBinFile.close();
        SubprocessWrapper::setExecutable(pyBin);

        PackageMetadata pyMeta;
        pyMeta.package = "pyscript";
        pyMeta.mainExecutable = "usr/bin/pyscript.py";
        pyMeta.executables = {"usr/bin/pyscript.py"};

        builder.createAppRun(pyAppDirPath, pyMeta);
        const QString pyAppRunPath = pyAppDirPath + "/AppRun";
        EMP_ASSERT(QFile::exists(pyAppRunPath), "Python AppRun created");

        QFile pyAppRunFile(pyAppRunPath);
        pyAppRunFile.open(QIODevice::ReadOnly | QIODevice::Text);
        QString pyContent = QString::fromUtf8(pyAppRunFile.readAll());
        pyAppRunFile.close();

        // Check PYTHONPATH regex
        EMP_ASSERT(!trailingColonRegex.match(pyContent).hasMatch(), "Python AppRun contains no trailing colon");
        EMP_ASSERT(!leadingColonRegex.match(pyContent).hasMatch(), "Python AppRun contains no leading colon");
        EMP_ASSERT(pyContent.contains("${PYTHONPATH:+:${PYTHONPATH}}"), "Python AppRun has safe ${PYTHONPATH:+:${PYTHONPATH}}");

        // Execution under Bash with unset PYTHONPATH
        QString pyEvalScript = QString(
            "HERE='%1'; "
            "eval \"$(sed '/^[[:space:]]*exec /d' '%2')\"; "
            "echo \"PY=$PYTHONPATH\"; "
        ).arg(pyAppDirPath, pyAppRunPath);

        QProcessEnvironment cleanEnv;
        QString pyOutput = runBashInEnv(pyEvalScript, cleanEnv);
        QString pyVal;
        for (const QString& line : pyOutput.split('\n')) {
            if (line.startsWith("PY=")) pyVal = line.mid(3);
        }

        EMP_ASSERT(!pyVal.isEmpty(), "PYTHONPATH evaluated in clean env");
        EMP_ASSERT(!pyVal.startsWith(":") && !pyVal.endsWith(":") && !pyVal.contains("::"),
                   "PYTHONPATH has no leading/trailing/double colons in clean env");
        EMP_ASSERT(!pyVal.split(':').contains(".") && !pyVal.split(':').contains(""),
                   "PYTHONPATH does not include CWD '.'");
    }
}

// ---------------------------------------------------------------------------
// TEST SUITE 2: Single Root .desktop File Contract
// ---------------------------------------------------------------------------
static void test_single_root_desktop_contract() {
    EMP_TEST("Single root .desktop file contract");

    QTemporaryDir tempDir;
    EMP_ASSERT(tempDir.isValid(), "Temporary directory created");
    const QString appDirPath = tempDir.filePath("AppDir");
    const QString extractedDir = tempDir.filePath("extracted");

    QDir().mkpath(appDirPath);
    QDir().mkpath(extractedDir + "/usr/share/applications");
    QDir().mkpath(extractedDir + "/usr/local/share/applications");
    QDir().mkpath(extractedDir + "/usr/bin");
    QDir().mkpath(extractedDir + "/usr/share/icons/hicolor/256x256/apps");

    // Extracted package has multiple desktop files:
    // 1. Canonical application desktop
    const QString canonDesktop = extractedDir + "/usr/share/applications/canonical-app.desktop";
    QFile fCanon(canonDesktop);
    fCanon.open(QIODevice::WriteOnly | QIODevice::Text);
    fCanon.write("[Desktop Entry]\nType=Application\nName=Canonical App\nExec=canonical-app\nIcon=canonical-app\nCategories=Utility;\n");
    fCanon.close();

    // 2. Secondary helper desktop
    const QString helperDesktop = extractedDir + "/usr/share/applications/canonical-helper.desktop";
    QFile fHelper(helperDesktop);
    fHelper.open(QIODevice::WriteOnly | QIODevice::Text);
    fHelper.write("[Desktop Entry]\nType=Application\nName=Helper\nExec=helper\nIcon=helper\nCategories=Utility;\n");
    fHelper.close();

    // 3. Rogue desktop inside usr/local/share
    const QString rogueInPackage = extractedDir + "/usr/local/share/applications/rogue-pkg.desktop";
    QFile fRoguePkg(rogueInPackage);
    fRoguePkg.open(QIODevice::WriteOnly | QIODevice::Text);
    fRoguePkg.write("[Desktop Entry]\nType=Application\nName=Rogue Pkg\nExec=rogue\n");
    fRoguePkg.close();

    // 4. Executable
    const QString binPath = extractedDir + "/usr/bin/canonical-app";
    QFile binFile(binPath);
    binFile.open(QIODevice::WriteOnly | QIODevice::Text);
    binFile.write("#!/bin/sh\nexit 0\n");
    binFile.close();
    SubprocessWrapper::setExecutable(binPath);

    // 5. Icon
    const QString iconPath = extractedDir + "/usr/share/icons/hicolor/256x256/apps/canonical-app.png";
    QFile iconFile(iconPath);
    iconFile.open(QIODevice::WriteOnly);
    iconFile.write("dummy-png-data");
    iconFile.close();

    // Plant MULTIPLE pre-existing rogue desktop files directly at AppDir root
    const QStringList rootRogues = {
        appDirPath + "/rogue_root_1.desktop",
        appDirPath + "/rogue_root_2.desktop",
        appDirPath + "/malicious.desktop",
        appDirPath + "/old_canonical.desktop"
    };
    for (const QString& rogue : rootRogues) {
        QFile rf(rogue);
        rf.open(QIODevice::WriteOnly | QIODevice::Text);
        rf.write("[Desktop Entry]\nType=Application\nName=Rogue\nExec=rogue\n");
        rf.close();
    }

    PackageMetadata meta;
    meta.package = "canonical-app";
    meta.mainExecutable = binPath;
    meta.executables = {binPath};
    meta.iconPath = iconPath;

    AppDirBuilder builder;
    bool buildSuccess = builder.buildAppDir(appDirPath, extractedDir, meta, {});
    EMP_ASSERT(buildSuccess, "AppDirBuilder::buildAppDir completed successfully");

    // Check all .desktop files at AppDir root
    QStringList rootDesktops = QDir(appDirPath).entryList({"*.desktop"}, QDir::Files);
    std::cout << "  Root .desktop files count: " << rootDesktops.size() << std::endl;
    for (const QString& d : rootDesktops) {
        std::cout << "    Found root .desktop: " << d.toStdString() << std::endl;
    }

    EMP_ASSERT(rootDesktops.size() == 1, "AppDir root contains STRICTLY ONE .desktop file");
    EMP_ASSERT(rootDesktops.first() == "canonical-app.desktop", "Single root .desktop is canonical-app.desktop");

    // Verify all planted rogue files at root were completely eliminated
    for (const QString& rogue : rootRogues) {
        EMP_ASSERT(!QFile::exists(rogue), QString("Planted rogue %1 was removed").arg(QFileInfo(rogue).fileName()).toStdString().c_str());
    }
}

// ---------------------------------------------------------------------------
// TEST SUITE 3: Relative .DirIcon Symlink Contract
// ---------------------------------------------------------------------------
static void test_relative_diricon_symlink() {
    EMP_TEST("Relative .DirIcon symlink verification via readlink");

    QTemporaryDir tempDir;
    EMP_ASSERT(tempDir.isValid(), "Temporary directory created");
    const QString appDirPath = tempDir.filePath("AppDir");
    const QString extractedDir = tempDir.filePath("extracted");

    QDir().mkpath(appDirPath);
    QDir().mkpath(extractedDir + "/usr/share/applications");
    QDir().mkpath(extractedDir + "/usr/bin");
    QDir().mkpath(extractedDir + "/usr/share/icons/hicolor/128x128/apps");

    const QString desktop = extractedDir + "/usr/share/applications/demo.desktop";
    QFile fDesk(desktop);
    fDesk.open(QIODevice::WriteOnly | QIODevice::Text);
    fDesk.write("[Desktop Entry]\nType=Application\nName=Demo\nExec=demo\nIcon=demo\nCategories=Utility;\n");
    fDesk.close();

    const QString icon = extractedDir + "/usr/share/icons/hicolor/128x128/apps/demo.png";
    QFile fIcon(icon);
    fIcon.open(QIODevice::WriteOnly);
    fIcon.write("png-bytes-here");
    fIcon.close();

    const QString exec = extractedDir + "/usr/bin/demo";
    QFile fExec(exec);
    fExec.open(QIODevice::WriteOnly | QIODevice::Text);
    fExec.write("#!/bin/sh\nexit 0\n");
    fExec.close();
    SubprocessWrapper::setExecutable(exec);

    PackageMetadata meta;
    meta.package = "demo";
    meta.mainExecutable = exec;
    meta.executables = {exec};
    meta.iconPath = icon;

    AppDirBuilder builder;
    bool success = builder.buildAppDir(appDirPath, extractedDir, meta, {});
    EMP_ASSERT(success, "AppDirBuilder::buildAppDir succeeded");

    const QString dirIconPath = appDirPath + "/.DirIcon";
    EMP_ASSERT(QFile::exists(dirIconPath), ".DirIcon exists at AppDir root");

    // Inspect with POSIX lstat and readlink
    struct stat st;
    int lstatRes = lstat(dirIconPath.toUtf8().constData(), &st);
    EMP_ASSERT(lstatRes == 0, "lstat(.DirIcon) succeeded");
    EMP_ASSERT(S_ISLNK(st.st_mode), ".DirIcon is a symbolic link (S_ISLNK)");

    char linkBuf[1024] = {0};
    ssize_t linkLen = readlink(dirIconPath.toUtf8().constData(), linkBuf, sizeof(linkBuf) - 1);
    EMP_ASSERT(linkLen > 0, "readlink(.DirIcon) returned > 0 bytes");

    QString targetStr = QString::fromUtf8(linkBuf, linkLen);
    std::cout << "  readlink raw target: " << targetStr.toStdString() << std::endl;

    EMP_ASSERT(!targetStr.startsWith("/"), "readlink target is NOT an absolute path");
    EMP_ASSERT(targetStr == "demo.png", "readlink target is exactly 'demo.png'");

    // Verify target file actually exists in AppDir root
    const QString resolvedTarget = appDirPath + "/" + targetStr;
    EMP_ASSERT(QFile::exists(resolvedTarget), QString("Target file exists in AppDir: %1").arg(resolvedTarget).toStdString().c_str());
}

// ---------------------------------------------------------------------------
// TEST SUITE 4: Active glibc CWD Preload Proof
// ---------------------------------------------------------------------------
static void test_active_glibc_cwd_prevention() {
    EMP_TEST("Active dynamic linker CWD preload prevention proof");

    QTemporaryDir attackDir;
    QTemporaryDir victimDir;
    QTemporaryDir appDir;
    EMP_ASSERT(attackDir.isValid() && victimDir.isValid() && appDir.isValid(), "Temp directories valid");

    const QString attackPath = attackDir.path();
    const QString victimPath = victimDir.path();
    const QString appDirPath = appDir.path();

    // 1. Build a dummy shared library in attackPath
    const QString cLibSrc = attackPath + "/libcwdprobe.c";
    QFile cLibFile(cLibSrc);
    cLibFile.open(QIODevice::WriteOnly | QIODevice::Text);
    cLibFile.write("int probe_function() { return 42; }\n");
    cLibFile.close();

    QProcess compileLib;
    compileLib.start("gcc", {"-shared", "-fPIC", cLibSrc, "-o", attackPath + "/libcwdprobe.so"});
    compileLib.waitForFinished(5000);
    EMP_ASSERT(compileLib.exitCode() == 0, "Compiled libcwdprobe.so in attack directory");

    // 2. Build victim binary in victimPath (outside attackPath, NO rpath)
    const QString victimSrc = victimPath + "/victim.c";
    QFile vFile(victimSrc);
    vFile.open(QIODevice::WriteOnly | QIODevice::Text);
    vFile.write("extern int probe_function(); int main() { return probe_function(); }\n");
    vFile.close();

    QProcess compileVictim;
    compileVictim.start("gcc", {victimSrc, "-L" + attackPath, "-lcwdprobe", "-Wl,--no-as-needed", "-o", victimPath + "/victim"});
    compileVictim.waitForFinished(5000);
    EMP_ASSERT(compileVictim.exitCode() == 0, "Compiled victim binary");

    // 3. Generate AppRun using AppDirBuilder
    QDir().mkpath(appDirPath + "/usr/bin");
    QDir().mkpath(appDirPath + "/usr/lib");
    PackageMetadata meta;
    meta.package = "dummy";
    meta.mainExecutable = "usr/bin/dummy";
    meta.executables = {"usr/bin/dummy"};

    AppDirBuilder builder;
    builder.createAppRun(appDirPath, meta);
    const QString appRunPath = appDirPath + "/AppRun";
    EMP_ASSERT(QFile::exists(appRunPath), "AppRun generated for CWD test");

    // 4. Test SECURE execution: run victim binary from attackPath with AppRun environment
    // Since AppRun contains NO trailing colon, glibc MUST NOT load libcwdprobe.so from CWD
    QProcess safeProc;
    safeProc.setProcessEnvironment(QProcessEnvironment()); // completely clean environment
    safeProc.setWorkingDirectory(attackPath);
    QString safeCmd = QString("HERE='%1'; eval \"$(sed '/^[[:space:]]*exec /d' '%2')\"; '%3/victim'").arg(appDirPath, appRunPath, victimPath);
    safeProc.start("bash", {"-c", safeCmd});
    safeProc.waitForFinished(5000);

    EMP_ASSERT(safeProc.exitCode() != 0, "Victim execution FAILED under secure AppRun (CWD library NOT loaded)");
    QString safeStderr = QString::fromUtf8(safeProc.readAllStandardError());
    EMP_ASSERT(safeStderr.contains("cannot open shared object file") || safeStderr.contains("error while loading shared libraries"),
               "Dynamic linker reported missing library as expected under secure AppRun");

    // 5. Test INSECURE contrast: artificially append trailing colon to prove the vulnerability would have triggered
    QProcess exploitProc;
    exploitProc.setProcessEnvironment(QProcessEnvironment());
    exploitProc.setWorkingDirectory(attackPath);
    QString exploitCmd = QString("HERE='%1'; eval \"$(sed '/^[[:space:]]*exec /d' '%2')\"; export LD_LIBRARY_PATH=\"${LD_LIBRARY_PATH}:\" && '%3/victim'").arg(appDirPath, appRunPath, victimPath);
    exploitProc.start("bash", {"-c", exploitCmd});
    exploitProc.waitForFinished(5000);

    EMP_ASSERT(exploitProc.exitCode() == 42, "Trailing colon intentionally reproduced CWD library preload (exit 42)");
    std::cout << "  [EMPIRICAL PROOF] Without trailing colon: CWD load rejected. With trailing colon: CWD loaded." << std::endl;
}

// ---------------------------------------------------------------------------
// TEST SUITE 5: SVG Icon & Relative Symlink Contract
// ---------------------------------------------------------------------------
static void test_svg_icon_relative_diricon() {
    EMP_TEST("SVG icon .DirIcon relative symlink verification");

    QTemporaryDir tempDir;
    EMP_ASSERT(tempDir.isValid(), "Temporary directory created");
    const QString appDirPath = tempDir.filePath("AppDir");
    const QString extractedDir = tempDir.filePath("extracted");

    QDir().mkpath(appDirPath);
    QDir().mkpath(extractedDir + "/usr/share/applications");
    QDir().mkpath(extractedDir + "/usr/bin");
    QDir().mkpath(extractedDir + "/usr/share/icons/hicolor/scalable/apps");

    const QString desktop = extractedDir + "/usr/share/applications/svgapp.desktop";
    QFile fDesk(desktop);
    fDesk.open(QIODevice::WriteOnly | QIODevice::Text);
    fDesk.write("[Desktop Entry]\nType=Application\nName=SvgApp\nExec=svgapp\nIcon=svgapp\nCategories=Graphics;\n");
    fDesk.close();

    const QString icon = extractedDir + "/usr/share/icons/hicolor/scalable/apps/svgapp.svg";
    QFile fIcon(icon);
    fIcon.open(QIODevice::WriteOnly);
    fIcon.write("<svg><rect width='10' height='10'/></svg>");
    fIcon.close();

    const QString exec = extractedDir + "/usr/bin/svgapp";
    QFile fExec(exec);
    fExec.open(QIODevice::WriteOnly | QIODevice::Text);
    fExec.write("#!/bin/sh\nexit 0\n");
    fExec.close();
    SubprocessWrapper::setExecutable(exec);

    PackageMetadata meta;
    meta.package = "svgapp";
    meta.mainExecutable = exec;
    meta.executables = {exec};
    meta.iconPath = icon;

    AppDirBuilder builder;
    bool success = builder.buildAppDir(appDirPath, extractedDir, meta, {});
    EMP_ASSERT(success, "AppDirBuilder::buildAppDir with SVG succeeded");

    const QString dirIconPath = appDirPath + "/.DirIcon";
    EMP_ASSERT(QFile::exists(dirIconPath), ".DirIcon exists for SVG app");

    char linkBuf[1024] = {0};
    ssize_t linkLen = readlink(dirIconPath.toUtf8().constData(), linkBuf, sizeof(linkBuf) - 1);
    EMP_ASSERT(linkLen > 0, "readlink(.DirIcon) returned > 0 for SVG");
    QString targetStr = QString::fromUtf8(linkBuf, linkLen);
    EMP_ASSERT(!targetStr.startsWith("/"), "SVG .DirIcon target is relative");
    EMP_ASSERT(targetStr == "svgapp.svg", "SVG .DirIcon target is exactly 'svgapp.svg'");
    EMP_ASSERT(QFile::exists(appDirPath + "/" + targetStr), "Target SVG file exists in AppDir root");
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main() {
    std::cout << "Starting Empirical M4 Stress Test Harness..." << std::endl;

    test_apprun_environment_variables();
    test_single_root_desktop_contract();
    test_relative_diricon_symlink();
    test_active_glibc_cwd_prevention();
    test_svg_icon_relative_diricon();

    std::cout << "\n========================================\n"
              << "SUMMARY: " << g_passCount << " passed, " << g_failCount << " failed.\n"
              << "========================================" << std::endl;

    return (g_failCount == 0) ? 0 : 1;
}
