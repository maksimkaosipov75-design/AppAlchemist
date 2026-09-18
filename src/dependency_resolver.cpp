#include "dependency_resolver.h"
#include "repository_browser.h"
#include "utils.h"
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QTextStream>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QDirIterator>
#include <QHash>
#include <QQueue>
#include <QDebug>
#include <QEventLoop>
#include <QTimer>
#include <QMap>

DependencyResolver::DependencyResolver(QObject* parent)
    : QObject(parent)
    , m_browser(new RepositoryBrowser(this))
{
    initializeExcludePatterns();
    initializeSonameExcludePatterns();
    
    connect(m_browser, &RepositoryBrowser::log, this, &DependencyResolver::log);
}

DependencyResolver::~DependencyResolver() {
}

void DependencyResolver::initializeExcludePatterns() {
    // Core system libraries that should not be bundled
    m_excludePatterns = {
        // Core C library and runtime
        "libc6", "libc-bin", "libc.so", "libm.so", "libdl.so", "librt.so",
        "libpthread.so", "ld-linux", "linux-vdso",
        
        // GCC runtime
        "libgcc", "libstdc++", "libgomp",
        
        // X11 and display
        "libx11", "libxext", "libxrender", "libxrandr", "libxi", "libxcursor",
        "libxcomposite", "libxdamage", "libxfixes", "libxinerama",
        "libdrm", "libgl", "libegl", "libgbm", "libvulkan",
        "libwayland", "libxkb",
        
        // Mesa and graphics drivers
        "mesa", "nvidia", "amdgpu", "radeon", "intel",
        
        // D-Bus and system services
        "libdbus", "libsystemd", "libudev", "libpolkit",
        
        // Audio hardware access: ALSA plugins live outside the AppDir, so a
        // bundled libasound would look for them in the wrong prefix.
        "libasound", "pipewire", "libjack",
        
        // Core utilities
        "coreutils", "base-files", "bash", "dash",
        
        // Font stack: a bundled fontconfig cannot read the host font cache.
        "libfontconfig", "libfreetype",
        
        "ca-certificates",
        
        // Kernel modules
        "linux-image", "linux-headers"
    };
    
    // Add patterns from settings
    for (const QString& pattern : m_settings.excludePatterns) {
        m_excludePatterns.insert(pattern.toLower());
    }
}

void DependencyResolver::initializeSonameExcludePatterns() {
    // Matched as a prefix against the lowercased soname reported by ldd.
    // Everything NOT listed here gets copied into the AppDir, which is the
    // opposite of the old behaviour (bundle nothing unless ldd said "not
    // found") and is what makes the result runnable on a foreign system.
    m_sonameExcludePatterns = {
        // glibc and the dynamic loader must come from the host kernel/libc pair
        "ld-linux", "ld.so", "libc.so", "libm.so", "libdl.so", "librt.so",
        "libpthread.so", "libresolv.so", "libnsl.so", "libutil.so",
        "libcrypt.so", "libanl.so", "libthread_db.so", "libnss_",
        "linux-vdso", "libmvec.so",

        // GCC/C++ runtime: always present, and mixing versions breaks the ABI
        "libgcc_s.so", "libstdc++.so", "libgomp.so",

        // Graphics stack: must match the host GPU driver
        "libgl.so", "libglx.so", "libegl.so", "libgldispatch.so",
        "libopengl.so", "libglesv1", "libglesv2", "libglapi.so",
        "libgbm.so", "libdrm.so", "libvulkan.so", "libnvidia", "libcuda.so",
        "libxcb-dri", "libxcb-glx", "libx11.so", "libx11-xcb.so", "libxext.so",

        // Host session services and security modules
        "libdbus-1.so", "libsystemd.so", "libudev.so", "libselinux.so",
        "libapparmor.so", "libcap.so",

        // Font stack: bundled caches are incompatible with the host's
        "libfontconfig.so", "libfreetype.so",

        // Sound hardware access
        "libasound.so"
    };
}

bool DependencyResolver::shouldExcludeSoname(const QString& soname) const {
    const QString lowered = QFileInfo(soname).fileName().toLower();
    if (lowered.isEmpty()) {
        return true;
    }

    for (const QString& pattern : m_sonameExcludePatterns) {
        if (lowered.startsWith(pattern)) {
            return true;
        }
    }

    for (const QString& pattern : m_settings.excludePatterns) {
        if (!pattern.isEmpty() && lowered.startsWith(pattern.toLower())) {
            return true;
        }
    }

    return false;
}

QString LibraryBundleReport::summary() const {
    if (!ran) {
        return QStringLiteral("library bundling skipped");
    }

    QString text = QString("scanned %1 ELF objects, bundled %2 libraries, left %3 to the host")
                       .arg(scannedBinaries)
                       .arg(bundled.size())
                       .arg(skippedSystemLibraries);
    if (!unresolved.isEmpty()) {
        text += QString(", %1 unresolved (%2)")
                    .arg(unresolved.size())
                    .arg(unresolved.mid(0, 8).join(", "));
    }
    if (!patchelfAvailable) {
        text += "; patchelf unavailable, relying on LD_LIBRARY_PATH only";
    }
    return text;
}

namespace {

// Reads the ELF identification bytes. Returns 0 when the file is not an ELF
// object, 32 or 64 for the respective ELF class.
int elfClassOf(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return 0;
    }

    char header[5] = {};
    if (file.read(header, 5) != 5) {
        return 0;
    }

    if (header[0] != 0x7f || header[1] != 'E' || header[2] != 'L' || header[3] != 'F') {
        return 0;
    }

    return header[4] == 2 ? 64 : 32;
}

// Parses one ldd output line into (soname, resolved host path).
// Returns false for lines that carry no dependency (vdso, loader, "statically
// linked", blank lines).
bool parseLddLine(const QString& rawLine, QString& soname, QString& resolvedPath, bool& notFound) {
    const QString line = rawLine.trimmed();
    soname.clear();
    resolvedPath.clear();
    notFound = false;

    if (line.isEmpty() || line.startsWith("statically linked")) {
        return false;
    }

    const int arrow = line.indexOf("=>");
    if (arrow < 0) {
        // "linux-vdso.so.1 (0x...)" or "/lib64/ld-linux-x86-64.so.2 (0x...)"
        return false;
    }

    soname = line.left(arrow).trimmed();
    QString right = line.mid(arrow + 2).trimmed();

    if (right.startsWith("not found")) {
        notFound = true;
        return !soname.isEmpty();
    }

    // Strip the trailing load address "(0x00007f....)"
    const int addressStart = right.lastIndexOf(" (0x");
    if (addressStart > 0) {
        right = right.left(addressStart).trimmed();
    }

    resolvedPath = right;
    return !soname.isEmpty() && !resolvedPath.isEmpty();
}

// Resolves a bare soname (e.g. "libgnutls.so.30") to a real file on the host,
// following symlinks so the versioned target is returned rather than a link.
QString resolveHostLibrary(const QString& soname) {
    static const QStringList searchPaths = {
        "/usr/lib/x86_64-linux-gnu",
        "/usr/lib/aarch64-linux-gnu",
        "/usr/lib64",
        "/usr/lib",
        "/lib/x86_64-linux-gnu",
        "/lib64",
        "/lib"
    };

    const QString fileName = QFileInfo(soname).fileName();
    if (fileName.isEmpty()) {
        return QString();
    }

    for (const QString& dir : searchPaths) {
        const QFileInfo candidate(QDir(dir).absoluteFilePath(fileName));
        if (!candidate.exists()) {
            continue;
        }
        const QString canonical = candidate.canonicalFilePath();
        if (!canonical.isEmpty()) {
            return canonical;
        }
    }

    return QString();
}

// Relative path expressed for an RPATH $ORIGIN entry, e.g. "../lib".
QString originRelativePath(const QString& fromDir, const QString& toDir) {
    const QString relative = QDir(fromDir).relativeFilePath(toDir);
    if (relative.isEmpty() || relative == ".") {
        return QStringLiteral("$ORIGIN");
    }
    return QString("$ORIGIN/%1").arg(relative);
}

} // end anonymous namespace

// Executes ldd inside an unshared bubblewrap sandbox if available; otherwise falls back
// to passive DT_NEEDED static inspection via readelf -d (never executes binary code directly).
QStringList DependencyResolver::runSafeLdd(const QString& binaryPath, const QProcessEnvironment& env) {
    QStringList lddOutput;
    const QString absBinaryPath = QFileInfo(binaryPath).absoluteFilePath();

    const bool bwrapBypassed = qEnvironmentVariableIsSet("APPALCHEMIST_DISABLE_BWRAP") ||
                               env.contains("APPALCHEMIST_DISABLE_BWRAP");
    const QString bwrapPath = bwrapBypassed ? QString() : QStandardPaths::findExecutable("bwrap");

    if (!bwrapPath.isEmpty()) {
        QStringList bwrapArgs = {
            "--unshare-all",
            "--ro-bind", "/", "/",
            "--dev", "/dev",
            "--proc", "/proc",
            "--tmpfs", "/tmp"
        };

        // If target binary is under /tmp/ or /var/tmp/, bind-mount it read-only
        // to prevent --tmpfs /tmp from masking the binary
        if (absBinaryPath.startsWith("/tmp/") || absBinaryPath.startsWith("/var/tmp/")) {
            bwrapArgs << "--ro-bind" << absBinaryPath << absBinaryPath;
        }

        // If an enclosing directory (such as AppDir or extracted tree) is known under /tmp/ or /var/tmp/, bind-mount it
        QDir dir(QFileInfo(absBinaryPath).absolutePath());
        QString enclosingDir;
        while (!dir.isRoot()) {
            const QString path = dir.absolutePath();
            const QString name = dir.dirName();
            if (name.endsWith(".AppDir", Qt::CaseInsensitive) || name == "AppDir" ||
                path.contains("/appalchemist-") || path.contains("/staged_deps")) {
                enclosingDir = path;
                break;
            }
            if (!dir.cdUp()) break;
        }
        if (!enclosingDir.isEmpty() && (enclosingDir.startsWith("/tmp/") || enclosingDir.startsWith("/var/tmp/"))) {
            bwrapArgs << "--ro-bind" << enclosingDir << enclosingDir;
        }

        // If env contains LD_LIBRARY_PATH, split by ':' and for each directory path located
        // under /tmp/ or /var/tmp/ that exists on disk, bind-mount it read-only
        QString ldLibraryPath = env.value("LD_LIBRARY_PATH");
        if (ldLibraryPath.isEmpty()) {
            ldLibraryPath = QProcessEnvironment::systemEnvironment().value("LD_LIBRARY_PATH");
        }
        if (!ldLibraryPath.isEmpty()) {
            const QStringList libDirs = ldLibraryPath.split(':', Qt::SkipEmptyParts);
            for (const QString& dirPath : libDirs) {
                const QString absDirPath = QFileInfo(dirPath).absoluteFilePath();
                if ((absDirPath.startsWith("/tmp/") || absDirPath.startsWith("/var/tmp/")) && QDir(absDirPath).exists()) {
                    bwrapArgs << "--ro-bind" << absDirPath << absDirPath;
                }
            }
        }

        bwrapArgs << "--" << "ldd" << absBinaryPath;

        const bool simulateFailure = qEnvironmentVariableIsSet("APPALCHEMIST_SIMULATE_BWRAP_FAILURE") ||
                                     env.contains("APPALCHEMIST_SIMULATE_BWRAP_FAILURE");
        if (!simulateFailure) {
            ProcessResult result = SubprocessWrapper::execute(bwrapPath, bwrapArgs, {}, 30000, env);
            if (result.success && !result.stdoutOutput.trimmed().isEmpty()) {
                lddOutput = result.stdoutOutput.split('\n', Qt::SkipEmptyParts);
            }
        }
    }

    // Automatic fallback to passive DT_NEEDED parsing via readelf -d (never executes untrusted binary):
    // Triggers if bwrap is missing, bypassed, or if bwrap execution failed (e.g. exit code != 0,
    // empty output, or unprivileged user namespaces disabled)
    if (lddOutput.isEmpty()) {
        QString readelfCmd = QStandardPaths::findExecutable("readelf");
        if (readelfCmd.isEmpty()) {
            readelfCmd = QStandardPaths::findExecutable("llvm-readelf");
        }
        if (!readelfCmd.isEmpty()) {
            QProcessEnvironment readelfEnv = env;
            readelfEnv.insert("LC_ALL", "C");
            ProcessResult result = SubprocessWrapper::execute(readelfCmd, {"-d", absBinaryPath}, {}, 10000, readelfEnv);
            if (result.success) {
                QStringList searchDirs;
                QString ldLibraryPath = env.value("LD_LIBRARY_PATH");
                if (ldLibraryPath.isEmpty()) {
                    ldLibraryPath = QProcessEnvironment::systemEnvironment().value("LD_LIBRARY_PATH");
                }
                if (!ldLibraryPath.isEmpty()) {
                    searchDirs << ldLibraryPath.split(':', Qt::SkipEmptyParts);
                }
                const QString binDir = QFileInfo(absBinaryPath).absolutePath();
                if (!binDir.isEmpty()) {
                    searchDirs << binDir;
                    searchDirs << QDir(binDir).filePath("../lib");
                    searchDirs << QDir(binDir).filePath("../lib64");
                }
                searchDirs << "/usr/lib/x86_64-linux-gnu" << "/lib/x86_64-linux-gnu"
                           << "/usr/lib/aarch64-linux-gnu" << "/lib/aarch64-linux-gnu"
                           << "/usr/lib64" << "/lib64" << "/usr/lib" << "/lib";

                const QStringList rawLines = result.stdoutOutput.split('\n', Qt::SkipEmptyParts);
                for (const QString& rLine : rawLines) {
                    if (rLine.contains("(RPATH)") || rLine.contains("(RUNPATH)")) {
                        const int openBracket = rLine.indexOf('[');
                        const int closeBracket = rLine.indexOf(']', openBracket);
                        if (openBracket >= 0 && closeBracket > openBracket) {
                            QString rpathStr = rLine.mid(openBracket + 1, closeBracket - openBracket - 1).trimmed();
                            rpathStr.replace("$ORIGIN", binDir);
                            rpathStr.replace("${ORIGIN}", binDir);
                            const QStringList rpathDirs = rpathStr.split(':', Qt::SkipEmptyParts);
                            for (int i = rpathDirs.size() - 1; i >= 0; --i) {
                                searchDirs.prepend(rpathDirs[i]);
                            }
                        }
                    }
                }

                for (const QString& rLine : rawLines) {
                    if (!rLine.contains("(NEEDED)")) continue;
                    const int openBracket = rLine.indexOf('[');
                    const int closeBracket = rLine.indexOf(']', openBracket);
                    if (openBracket < 0 || closeBracket <= openBracket) continue;
                    const QString soname = rLine.mid(openBracket + 1, closeBracket - openBracket - 1).trimmed();
                    if (soname.isEmpty()) continue;

                    QString resolvedPath;
                    for (const QString& sDir : searchDirs) {
                        const QString candidate = sDir + "/" + soname;
                        if (QFileInfo::exists(candidate)) {
                            resolvedPath = candidate;
                            break;
                        }
                    }
                    if (!resolvedPath.isEmpty()) {
                        lddOutput.append(QString("\t%1 => %2 (0x0)").arg(soname, resolvedPath));
                    } else {
                        lddOutput.append(QString("\t%1 => not found").arg(soname));
                    }
                }
            }
        }
    }
    return lddOutput;
}

namespace {

bool anySonameStartsWith(const QStringList& sonames, const QString& prefix) {
    for (const QString& soname : sonames) {
        if (soname.startsWith(prefix, Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

// Copies a host directory tree into the AppDir and collects any ELF objects it
// contains, so the caller can bundle their dependencies as well.
bool copyModuleTree(const QString& sourceDir,
                    const QString& destinationDir,
                    QStringList& newElfObjects) {
    if (!QDir(sourceDir).exists()) {
        return false;
    }

    if (!SubprocessWrapper::copyDirectory(sourceDir, destinationDir)) {
        return false;
    }

    QDirIterator it(destinationDir, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString candidate = it.next();
        if (candidate.endsWith(".so") || candidate.contains(".so.")) {
            newElfObjects << candidate;
        }
    }

    return true;
}

// First existing match of <dir>/<relative> across the candidate library roots.
QString findHostDirectory(const QStringList& roots, const QString& relative) {
    for (const QString& root : roots) {
        const QString candidate = QDir(root).absoluteFilePath(relative);
        if (QDir(candidate).exists()) {
            return candidate;
        }
    }
    return QString();
}

}

void DependencyResolver::bundleRuntimeModules(const QString& appDirPath,
                                              const QStringList& bundledSonames,
                                              const QStringList& hostLibraryDirs,
                                              QStringList& newElfObjects) {
    if (bundledSonames.isEmpty()) {
        return;
    }

    const QDir appDir(appDirPath);
    const QString appLibDir = appDir.absoluteFilePath("usr/lib");

    // Candidate host roots: wherever the bundled libraries came from, plus the
    // usual multiarch and lib64 locations.
    QStringList roots = hostLibraryDirs;
    for (const QString& fallback : {QStringLiteral("/usr/lib"),
                                    QStringLiteral("/usr/lib64"),
                                    QStringLiteral("/usr/lib/x86_64-linux-gnu"),
                                    QStringLiteral("/usr/lib/aarch64-linux-gnu")}) {
        if (!roots.contains(fallback)) {
            roots << fallback;
        }
    }

    // gdk-pixbuf image loaders. The cache file stores absolute paths, so it is
    // written with an @APPDIR@ placeholder that AppRun expands at startup.
    if (anySonameStartsWith(bundledSonames, "libgdk_pixbuf-2.0")) {
        const QString hostLoaders = findHostDirectory(roots, "gdk-pixbuf-2.0/2.10.0/loaders");
        if (!hostLoaders.isEmpty()) {
            const QString destination =
                QDir(appLibDir).absoluteFilePath("gdk-pixbuf-2.0/2.10.0/loaders");
            if (copyModuleTree(hostLoaders, destination, newElfObjects)) {
                emit log("  Bundled gdk-pixbuf loaders.");

                const QString cacheTemplate =
                    QDir(appLibDir).absoluteFilePath("gdk-pixbuf-2.0/2.10.0/loaders.cache.in");
                const QString queryTool = QStandardPaths::findExecutable("gdk-pixbuf-query-loaders");
                const QString hostCache = QFileInfo(hostLoaders).absolutePath() + "/loaders.cache";

                QString cacheContent;
                if (!queryTool.isEmpty()) {
                    QProcessEnvironment queryEnv = QProcessEnvironment::systemEnvironment();
                    queryEnv.insert("GDK_PIXBUF_MODULEDIR", destination);
                    const ProcessResult queryResult =
                        SubprocessWrapper::execute(queryTool, {}, {}, 30000, queryEnv);
                    if (queryResult.success) {
                        cacheContent = queryResult.stdoutOutput;
                    }
                }

                if (cacheContent.isEmpty() && QFileInfo::exists(hostCache)) {
                    QFile hostCacheFile(hostCache);
                    if (hostCacheFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
                        cacheContent = QString::fromUtf8(hostCacheFile.readAll());
                        hostCacheFile.close();
                        cacheContent.replace(QFileInfo(hostLoaders).absoluteFilePath(), destination);
                    }
                }

                if (!cacheContent.isEmpty()) {
                    cacheContent.replace(destination, "@APPDIR@/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders");
                    QFile cacheFile(cacheTemplate);
                    if (cacheFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
                        cacheFile.write(cacheContent.toUtf8());
                        cacheFile.close();
                    }
                }
            }
        }
    }

    // GIO modules (GVFS, TLS backends, ...).
    if (anySonameStartsWith(bundledSonames, "libgio-2.0")) {
        const QString hostGio = findHostDirectory(roots, "gio/modules");
        if (!hostGio.isEmpty() &&
            copyModuleTree(hostGio, QDir(appLibDir).absoluteFilePath("gio/modules"), newElfObjects)) {
            emit log("  Bundled GIO modules.");
        }
    }

    // GTK input methods and print backends.
    for (const QString& gtkVersion : {QStringLiteral("gtk-3.0/3.0.0"), QStringLiteral("gtk-4.0/4.0.0")}) {
        const QString gtkSoname = gtkVersion.startsWith("gtk-3") ? "libgtk-3" : "libgtk-4";
        if (!anySonameStartsWith(bundledSonames, gtkSoname)) {
            continue;
        }
        for (const QString& subdir : {QStringLiteral("immodules"), QStringLiteral("printbackends")}) {
            const QString hostDir = findHostDirectory(roots, QString("%1/%2").arg(gtkVersion, subdir));
            if (hostDir.isEmpty()) {
                continue;
            }
            const QString destination =
                QDir(appLibDir).absoluteFilePath(QString("%1/%2").arg(gtkVersion, subdir));
            if (copyModuleTree(hostDir, destination, newElfObjects)) {
                emit log(QString("  Bundled %1/%2.").arg(gtkVersion, subdir));
            }
        }
    }

    // GSettings schemas. Two things go wrong without this: a bundled GIO finds
    // none of the desktop's schemas, and a package that ships its own schema
    // has it in source form only. GSettings reads the compiled file, so an
    // application asking for its settings aborts with "Settings schema ... is
    // not installed" - which is how most GTK, GNOME and MATE programs failed.
    const QString schemaDir = appDir.absoluteFilePath("usr/share/glib-2.0/schemas");
    const bool bundlesGlib = anySonameStartsWith(bundledSonames, "libgio-2.0") ||
                             anySonameStartsWith(bundledSonames, "libgtk-");
    const bool packageShipsSchemas =
        QDir(schemaDir).exists() &&
        !QDir(schemaDir).entryList({"*.gschema.xml", "*.enums.xml"}, QDir::Files).isEmpty();

    if (bundlesGlib && QDir("/usr/share/glib-2.0/schemas").exists()) {
        QDir().mkpath(schemaDir);
        // The package's own schemas win: only what is missing comes from the host.
        const QDir hostSchemas("/usr/share/glib-2.0/schemas");
        for (const QFileInfo& entry : hostSchemas.entryInfoList(QDir::Files)) {
            const QString target = QDir(schemaDir).absoluteFilePath(entry.fileName());
            if (!QFileInfo::exists(target)) {
                SubprocessWrapper::copyFile(entry.absoluteFilePath(), target);
            }
        }
        emit log("  Bundled GSettings schemas.");
    }

    if (bundlesGlib || packageShipsSchemas) {
        const QDir schemas(schemaDir);
        if (schemas.exists() && !schemas.entryList({"*.gschema.xml"}, QDir::Files).isEmpty()) {
            const QString compiler = QStandardPaths::findExecutable("glib-compile-schemas");
            if (compiler.isEmpty()) {
                emit log("  WARNING: glib-compile-schemas not found; settings schemas stay uncompiled");
            } else if (SubprocessWrapper::execute(compiler, {schemaDir}, {}, 60000).success) {
                emit log("  Compiled GSettings schemas.");
            } else {
                emit log("  WARNING: failed to compile GSettings schemas");
            }
        }
    }
}

LibraryBundleReport DependencyResolver::bundleSystemLibraries(const QString& appDirPath) {
    LibraryBundleReport report;

    const QDir appDir(appDirPath);
    if (!appDir.exists()) {
        emit log("Library bundling skipped: AppDir does not exist.");
        return report;
    }

    report.ran = true;
    emit log("=== Bundling shared libraries into AppDir ===");

    const QString canonicalAppDir = QFileInfo(appDirPath).canonicalFilePath();
    const QString lib64Dir = appDir.absoluteFilePath("usr/lib");
    const QString lib32Dir = appDir.absoluteFilePath("usr/lib32");
    QDir().mkpath(lib64Dir);

    // ldd must see the libraries we already placed in the AppDir, otherwise it
    // reports them as host libraries and we copy them a second time.
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    QStringList ldPath = {lib64Dir, lib32Dir};
    const QString existingLdPath = env.value("LD_LIBRARY_PATH");
    if (!existingLdPath.isEmpty()) {
        ldPath << existingLdPath;
    }
    env.insert("LD_LIBRARY_PATH", ldPath.join(":"));

    report.patchelfAvailable = !QStandardPaths::findExecutable("patchelf").isEmpty();
    if (!report.patchelfAvailable) {
        emit log("  patchelf not found: RPATHs will not be rewritten (AppRun still sets LD_LIBRARY_PATH).");
    }

    // Seed the work list with every ELF object already inside the AppDir.
    QQueue<QString> pending;
    QSet<QString> queued;
    QDirIterator it(appDirPath, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString candidate = it.next();
        if (elfClassOf(candidate) == 0) {
            continue;
        }
        if (!queued.contains(candidate)) {
            queued.insert(candidate);
            pending.enqueue(candidate);
        }
    }

    if (pending.isEmpty()) {
        emit log("  No ELF objects found in AppDir; nothing to bundle.");
        return report;
    }

    QSet<QString> handledSonames;
    QSet<QString> skippedSonames;
    QSet<QString> unresolvedSonames;
    QStringList bundledPaths;
    QSet<QString> hostLibraryDirs;

    // Libraries the package ships in a private directory (for example
    // usr/lib/<app>/lib/libfoo.so). ldd cannot resolve them because the
    // executable was staged away from its original location and its
    // $ORIGIN-relative RPATH no longer points at them, so index them by file
    // name and re-attach the directory through RPATH further down.
    QHash<QString, QString> appDirLibraryIndex;
    {
        QDirIterator libIt(appDirPath, {"*.so", "*.so.*"}, QDir::Files,
                           QDirIterator::Subdirectories);
        while (libIt.hasNext()) {
            const QString candidate = libIt.next();
            const QString dir = QFileInfo(candidate).absolutePath();

            // Skip what this routine and the module copier put there
            // themselves: host libraries land directly in usr/lib, and host
            // loadable-module trees in a handful of well known subdirectories.
            // Everything else under usr/lib belongs to the package — private
            // runtimes commonly live in usr/lib/<app>/lib, and excluding the
            // whole subtree would hide exactly the libraries this index exists
            // to find.
            if (dir == lib64Dir || dir == lib32Dir) {
                continue;
            }
            static const QStringList hostModuleTrees = {
                "gio", "gtk-3.0", "gtk-4.0", "gdk-pixbuf-2.0",
                "girepository-1.0", "qt6", "qt5"
            };
            bool insideHostModuleTree = false;
            for (const QString& root : hostModuleTrees) {
                if (dir.startsWith(lib64Dir + "/" + root + "/") || dir == lib64Dir + "/" + root ||
                    dir.startsWith(lib32Dir + "/" + root + "/") || dir == lib32Dir + "/" + root) {
                    insideHostModuleTree = true;
                    break;
                }
            }
            if (insideHostModuleTree) {
                continue;
            }
            const QString name = QFileInfo(candidate).fileName();
            if (!appDirLibraryIndex.contains(name)) {
                appDirLibraryIndex.insert(name, candidate);
            }
        }
    }

    // binary -> extra directories that must end up on its RPATH.
    QHash<QString, QSet<QString>> privateRpathDirs;

    // Directories that hold a package-provided library which also exists as a
    // host copy in usr/lib. Every patched binary gets them ahead of usr/lib so
    // the package's own runtime wins.
    QStringList packageRuntimeDirs;
    {
        QStringList privateDirs;
        for (auto it = appDirLibraryIndex.constBegin(); it != appDirLibraryIndex.constEnd(); ++it) {
            const QString dir = QFileInfo(it.value()).absolutePath();
            if (!privateDirs.contains(dir)) {
                privateDirs << dir;
            }

            // A host copy of the same soname staged into usr/lib would be found
            // first and is usually older than the one the package was built
            // against, so remove it and rely on the package's own library.
            // Without patchelf the private directory cannot be put on the
            // search path, so the host copy is the only thing that resolves
            // and has to stay.
            const QString shadowing = QDir(lib64Dir).absoluteFilePath(it.key());
            if (report.patchelfAvailable && QFileInfo::exists(shadowing) && QFile::remove(shadowing)) {
                emit log(QString("  using package-provided %1, removed host copy from usr/lib").arg(it.key()));
                if (!packageRuntimeDirs.contains(dir)) {
                    packageRuntimeDirs << dir;
                }
            }
        }
        packageRuntimeDirs.sort();
        if (!privateDirs.isEmpty()) {
            privateDirs.sort();
            env.insert("LD_LIBRARY_PATH", (privateDirs + ldPath).join(":"));
        }
    }

    // Pass 0 walks the dependency graph of the packaged binaries. Between the
    // passes we pull in the loadable-module trees of whatever got bundled, and
    // pass 1 resolves the dependencies of those modules.
    for (int pass = 0; pass < 2; ++pass) {
        // Breadth-first over the dependency graph: newly copied libraries are fed
        // back in so their own dependencies are bundled too.
        while (!pending.isEmpty()) {
            const QString binary = pending.dequeue();
            report.scannedBinaries++;

            const QStringList lines = runSafeLdd(binary, env);
            if (lines.isEmpty()) {
                continue;
            }
            for (const QString& line : lines) {
                QString soname;
                QString hostPath;
                bool notFound = false;
                if (!parseLddLine(line, soname, hostPath, notFound)) {
                    continue;
                }

                if (shouldExcludeSoname(soname)) {
                    if (!skippedSonames.contains(soname)) {
                        skippedSonames.insert(soname);
                        report.skippedSystemLibraries++;
                    }
                    continue;
                }

                if (notFound) {
                    const QString shipped = appDirLibraryIndex.value(QFileInfo(soname).fileName());
                    if (!shipped.isEmpty()) {
                        // Shipped by the package itself: keep it where it is and
                        // make the binary look for it there.
                        privateRpathDirs[binary].insert(QFileInfo(shipped).absolutePath());
                        if (!queued.contains(shipped)) {
                            queued.insert(shipped);
                            pending.enqueue(shipped);
                        }
                        continue;
                    }
                    unresolvedSonames.insert(soname);
                    continue;
                }

                const QString canonicalHostPath = QFileInfo(hostPath).canonicalFilePath();
                if (canonicalHostPath.isEmpty()) {
                    unresolvedSonames.insert(soname);
                    continue;
                }

                // Already provided from inside the AppDir.
                if (!canonicalAppDir.isEmpty() && canonicalHostPath.startsWith(canonicalAppDir + "/")) {
                    const QString providerDir = QFileInfo(canonicalHostPath).absolutePath();
                    if (providerDir != lib64Dir && providerDir != lib32Dir) {
                        // Shipped by the package in a private directory. Record
                        // it for the RPATH pass and make sure no host copy in
                        // usr/lib shadows it at runtime: the package was built
                        // against its own version, which is frequently newer
                        // than anything installed on the build host.
                        privateRpathDirs[binary].insert(providerDir);
                        const QString shadowing =
                            QDir(lib64Dir).absoluteFilePath(QFileInfo(soname).fileName());
                        if (report.patchelfAvailable && QFileInfo::exists(shadowing) &&
                            shadowing != canonicalHostPath) {
                            if (QFile::remove(shadowing)) {
                                emit log(QString("  using package-provided %1, removed host copy").arg(soname));
                                report.bundled.removeAll(soname);
                            }
                        }
                        if (!queued.contains(canonicalHostPath)) {
                            queued.insert(canonicalHostPath);
                            pending.enqueue(canonicalHostPath);
                        }
                    }
                    continue;
                }

                // The package ships its own copy of this soname (typically a
                // private runtime under opt/<app>/lib) but ldd still resolved
                // it against the host, so prefer the shipped one.
                const QString shippedByPackage = appDirLibraryIndex.value(QFileInfo(soname).fileName());
                if (!shippedByPackage.isEmpty()) {
                    privateRpathDirs[binary].insert(QFileInfo(shippedByPackage).absolutePath());
                    if (!queued.contains(shippedByPackage)) {
                        queued.insert(shippedByPackage);
                        pending.enqueue(shippedByPackage);
                    }
                    continue;
                }

                if (handledSonames.contains(soname)) {
                    continue;
                }
                handledSonames.insert(soname);

                // Never re-introduce a host copy of a library the package
                // provides itself; it was deliberately removed from usr/lib.
                if (appDirLibraryIndex.contains(QFileInfo(soname).fileName())) {
                    continue;
                }

                const int elfClass = elfClassOf(canonicalHostPath);
                const QString targetDir = (elfClass == 32) ? lib32Dir : lib64Dir;
                QDir().mkpath(targetDir);

                const QString destination = QDir(targetDir).absoluteFilePath(QFileInfo(soname).fileName());
                const QFileInfo destinationInfo(destination);
                if (destinationInfo.exists()) {
                    continue;
                }

                // An earlier staging step may have recreated a soname symlink
                // (libfoo.so.1 -> libfoo.so.1.2.3) without copying its target.
                // Such a link resolves to nothing, shadows the library at
                // runtime and makes QFile::copy() fail because the name is
                // already taken, so drop it and place the real file instead.
                if (destinationInfo.isSymLink() && !QFile::remove(destination)) {
                    emit log(QString("  WARNING: could not replace broken symlink %1").arg(destination));
                    unresolvedSonames.insert(soname);
                    continue;
                }

                if (!QFile::copy(canonicalHostPath, destination)) {
                    emit log(QString("  WARNING: failed to copy %1 from %2").arg(soname, canonicalHostPath));
                    unresolvedSonames.insert(soname);
                    continue;
                }

                QFile::setPermissions(destination,
                                      QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner |
                                      QFile::ReadGroup | QFile::ExeGroup |
                                      QFile::ReadOther | QFile::ExeOther);

                report.bundled << soname;
                bundledPaths << destination;
                hostLibraryDirs.insert(QFileInfo(canonicalHostPath).absolutePath());

                if (!queued.contains(destination)) {
                    queued.insert(destination);
                    pending.enqueue(destination);
                }
            }
        }

        if (pass == 0) {
            QStringList moduleObjects;
            QStringList searchDirs = hostLibraryDirs.values();
            searchDirs.sort();
            bundleRuntimeModules(appDirPath, report.bundled, searchDirs, moduleObjects);
            for (const QString& moduleObject : moduleObjects) {
                if (!queued.contains(moduleObject)) {
                    queued.insert(moduleObject);
                    pending.enqueue(moduleObject);
                }
            }
        }
    }

    // Final sweep: any symlink left in the library directories that does not
    // resolve would break the AppImage on a host that lacks the library, which
    // is exactly what bundling is meant to prevent. Fill it from the host when
    // possible, otherwise remove it and report the soname as unresolved.
    // Plugin trees keep libraries of their own, several directories deep, and
    // a link left dangling there fails just as loudly as one in the library
    // directory itself, so the whole bundle is swept.
    QFileInfoList brokenLinks;
    {
        QDirIterator walker(appDirPath,
                            QDir::Files | QDir::System | QDir::NoDotAndDotDot,
                            QDirIterator::Subdirectories);
        while (walker.hasNext()) {
            walker.next();
            const QFileInfo candidate = walker.fileInfo();
            if (candidate.isSymLink() && !candidate.exists()) {
                brokenLinks << candidate;
            }
        }
    }
    {
        for (const QFileInfo& entry : brokenLinks) {

            const QString brokenPath = entry.absoluteFilePath();
            const QString soname = entry.fileName();
            if (!QFile::remove(brokenPath)) {
                emit log(QString("  WARNING: could not remove broken symlink %1").arg(brokenPath));
                continue;
            }

            // If the package ships this library itself, the broken link was the
            // only thing standing in the way: the binaries reach the real file
            // through the RPATH entries added below.
            const QString shipped = appDirLibraryIndex.value(soname);
            if (!shipped.isEmpty() && report.patchelfAvailable) {
                const QString shippedDir = QFileInfo(shipped).absolutePath();
                if (!packageRuntimeDirs.contains(shippedDir)) {
                    packageRuntimeDirs << shippedDir;
                    packageRuntimeDirs.sort();
                }
                emit log(QString("  using package-provided %1").arg(soname));
                continue;
            }

            const QString hostPath = resolveHostLibrary(soname);

            if (hostPath.isEmpty() || !QFile::copy(hostPath, brokenPath)) {
                emit log(QString("  removed broken library symlink: %1").arg(soname));
                unresolvedSonames.insert(soname);
                continue;
            }

            QFile::setPermissions(brokenPath,
                                  QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner |
                                  QFile::ReadGroup | QFile::ExeGroup |
                                  QFile::ReadOther | QFile::ExeOther);
            if (!report.bundled.contains(soname)) {
                report.bundled << soname;
            }
            emit log(QString("  replaced broken symlink with host library: %1").arg(soname));
        }
    }

    report.unresolved = unresolvedSonames.values();
    report.unresolved.sort();
    report.bundled.sort();

    // Point every ELF object at the bundled library directory. RPATH entries
    // are appended, never replaced: packages under /opt often ship an $ORIGIN
    // based RPATH that is required for their own private libraries.
    if (report.patchelfAvailable) {
        for (const QString& binary : queued) {
            const QString binaryDir = QFileInfo(binary).absolutePath();

            // Package-provided runtimes first: their host counterparts were
            // removed from usr/lib, and they are what the binaries were built
            // against.
            QStringList wanted;
            QStringList preferredDirs = packageRuntimeDirs;
            const QSet<QString> binaryPrivateDirs = privateRpathDirs.value(binary);
            for (const QString& privateDir : binaryPrivateDirs) {
                if (!preferredDirs.contains(privateDir)) {
                    preferredDirs << privateDir;
                }
            }
            for (const QString& preferred : preferredDirs) {
                const QString relative = originRelativePath(binaryDir, preferred);
                if (!wanted.contains(relative)) {
                    wanted << relative;
                }
            }

            wanted << originRelativePath(binaryDir, lib64Dir);
            if (QDir(lib32Dir).exists()) {
                wanted << originRelativePath(binaryDir, lib32Dir);
            }

            const ProcessResult current =
                SubprocessWrapper::execute("patchelf", {"--print-rpath", binary}, {}, 10000);
            QStringList entries;
            if (current.success) {
                const QString existing = current.stdoutOutput.trimmed();
                if (!existing.isEmpty()) {
                    entries = existing.split(':', Qt::SkipEmptyParts);
                }
            }

            bool changed = false;
            for (const QString& entry : wanted) {
                if (!entries.contains(entry)) {
                    entries << entry;
                    changed = true;
                }
            }

            if (!changed) {
                continue;
            }

            const ProcessResult setResult = SubprocessWrapper::execute(
                "patchelf", {"--set-rpath", entries.join(":"), binary}, {}, 15000);
            if (!setResult.success) {
                // Non-fatal: LD_LIBRARY_PATH from AppRun still covers the lookup.
                qDebug() << "patchelf --set-rpath failed for" << binary
                         << setResult.stderrOutput.left(200);
            }
        }
    }

    m_resolvedLibraries.append(bundledPaths);

    emit log(QString("  %1").arg(report.summary()));
    if (!report.unresolved.isEmpty()) {
        emit log(QString("  WARNING: unresolved sonames: %1").arg(report.unresolved.join(", ")));
    }
    emit log("=== Library bundling finished ===");

    return report;
}

void DependencyResolver::setSettings(const DependencySettings& settings) {
    m_settings = settings;
    initializeExcludePatterns();
    initializeSonameExcludePatterns();
}

void DependencyResolver::requirePackages(const QStringList& names) {
    for (const QString& name : names) {
        m_requiredPackages.insert(name);
    }
}

bool DependencyResolver::shouldExclude(const QString& name) {
    // A package asked for because the loader could not find the library it
    // carries is never excluded: the host demonstrably does not have it, so
    // leaving it out is what breaks the bundle. The exclusion list matches on
    // substrings, and "dbus" in it was enough to drop libdbusmenu-qt5-2.
    if (m_requiredPackages.contains(name)) {
        return false;
    }

    if (!m_settings.excludeSystemLibs) {
        return false;
    }
    
    QString lowerName = name.toLower();
    
    for (const QString& pattern : m_excludePatterns) {
        if (lowerName.contains(pattern)) {
            return true;
        }
    }
    
    return false;
}

QStringList DependencyResolver::parseDependencies(const QString& controlFilePath) {
    QStringList depends;
    
    QFile file(controlFilePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        emit log(QString("ERROR: Cannot open control file: %1").arg(controlFilePath));
        return depends;
    }
    
    QTextStream in(&file);
    QString currentField;
    QString currentValue;
    
    while (!in.atEnd()) {
        QString line = in.readLine();
        
        if (line.isEmpty()) continue;
        
        if (line.startsWith(" ") || line.startsWith("\t")) {
            // Continuation line
            currentValue += " " + line.trimmed();
        } else {
            // Process previous field
            if (currentField == "Depends" || currentField == "Pre-Depends") {
                depends.append(parseDependsString(currentValue));
            } else if (currentField == "Recommends" && m_settings.includeRecommended) {
                depends.append(parseDependsString(currentValue));
            }
            
            // New field
            int colonPos = line.indexOf(':');
            if (colonPos > 0) {
                currentField = line.left(colonPos).trimmed();
                currentValue = line.mid(colonPos + 1).trimmed();
            }
        }
    }
    
    // Handle last field
    if (currentField == "Depends" || currentField == "Pre-Depends") {
        depends.append(parseDependsString(currentValue));
    } else if (currentField == "Recommends" && m_settings.includeRecommended) {
        depends.append(parseDependsString(currentValue));
    }
    
    file.close();
    
    // Remove duplicates and excluded packages
    QStringList filtered;
    QSet<QString> seen;
    
    for (const QString& dep : depends) {
        QString baseName = dep.split(' ').first().trimmed();
        if (!shouldExclude(baseName) && !seen.contains(baseName)) {
            filtered.append(baseName);
            seen.insert(baseName);
        }
    }
    
    return filtered;
}

QStringList DependencyResolver::parseDependsString(const QString& dependsStr) {
    QStringList result;
    
    if (dependsStr.isEmpty()) {
        return result;
    }
    
    // Dependencies are comma-separated
    QStringList deps = dependsStr.split(',');
    
    for (QString dep : deps) {
        dep = dep.trimmed();
        if (dep.isEmpty()) continue;
        
        // Handle alternatives (pkg1 | pkg2)
        if (dep.contains('|')) {
            QStringList alternatives = dep.split('|');
            // Take first alternative
            dep = alternatives.first().trimmed();
        }
        
        // Remove version constraints for the result
        QString name, version, op;
        parseVersionConstraint(dep, name, version, op);
        
        if (!name.isEmpty()) {
            result.append(name);
        }
    }
    
    return result;
}

void DependencyResolver::parseVersionConstraint(const QString& dep, QString& name, QString& version, QString& op) {
    // Format: package (>> 1.0) or package (>= 1.0) or package (= 1.0) etc.
    QRegularExpression regex(R"(^([^\s(]+)\s*(?:\(([<>=!]+)\s*([^\s)]+)\))?$)");
    QRegularExpressionMatch match = regex.match(dep.trimmed());
    
    if (match.hasMatch()) {
        name = match.captured(1).trimmed();
        op = match.captured(2).trimmed();
        version = match.captured(3).trimmed();
    } else {
        name = dep.trimmed();
        op.clear();
        version.clear();
    }
}

int DependencyResolver::removeDanglingSymlinks(const QString& root) {
    // A link that resolves to nothing is shipped as a broken file: the loader
    // fails on it, and packaging tools report it. Resources are copied in
    // after libraries are bundled, so this runs once more at the very end.
    int removed = 0;
    QDirIterator walker(root, QDir::Files | QDir::System | QDir::NoDotAndDotDot,
                        QDirIterator::Subdirectories);
    QStringList broken;
    while (walker.hasNext()) {
        walker.next();
        const QFileInfo candidate = walker.fileInfo();
        if (candidate.isSymLink() && !candidate.exists()) {
            broken << candidate.absoluteFilePath();
        }
    }
    for (const QString& path : broken) {
        if (QFile::remove(path)) {
            removed++;
        }
    }
    return removed;
}

QStringList DependencyResolver::packageNamesForSoname(const QString& soname) {
    // A distribution names a library package after the library it carries:
    // libdbusmenu-qt5.so.2 is shipped by libdbusmenu-qt5-2, libpng16.so.16 by
    // libpng16-16, libfoo.so.0 by libfoo0. The version may be joined directly
    // or with a hyphen, and Debian appends t64 to packages rebuilt for 64-bit
    // time. Guessing from the name reaches a library that the declared
    // dependencies only mention through several other packages.
    QStringList candidates;
    static const QRegularExpression pattern(QStringLiteral("^(.+)\\.so\\.([0-9]+)"));
    const QRegularExpressionMatch match = pattern.match(soname);
    if (!match.hasMatch()) {
        return candidates;
    }

    const QString stem = match.captured(1);
    const QString version = match.captured(2);
    // Package names are lower case whatever the library is called, so
    // libKF5Archive.so.5 is shipped by libkf5archive5.
    for (const QString& base : {stem + version, stem + "-" + version, stem}) {
        for (const QString& cased : {base, base.toLower()}) {
            for (const QString& name : {cased, cased + "t64"}) {
                if (!candidates.contains(name) && SubprocessWrapper::isSafePackageName(name)) {
                    candidates << name;
                }
            }
        }
    }
    return candidates;
}

QStringList DependencyResolver::selectRuntimePackages(const QString& aptOutput,
                                                     const QStringList& already) {
    // Two kinds of package matter at runtime, and they are not equally
    // replaceable. A missing shared library still gets found: the loader names
    // it and it is copied from the host. A missing interpreter module is
    // invisible until the program dies on its first import, and nothing else
    // can supply it - so modules are all taken, while plain libraries, which
    // are merely a convenience here, are capped to keep the bundle sane.
    constexpr int kLibraryLimit = 80;
    // Modules are kept generously rather than without limit: a closure that
    // ran away would spend the conversion downloading packages the
    // application never imports.
    constexpr int kModuleLimit = 150;

    QStringList modules;
    QStringList libraries;

    const QStringList lines = aptOutput.split('\n');
    for (const QString& line : lines) {
        const QString candidate = line.trimmed();
        if (candidate.isEmpty() || candidate.startsWith('|') || candidate.contains(':') ||
            candidate.contains(' ')) {
            continue;   // a dependency line or an architecture-qualified name
        }
        if (already.contains(candidate) || modules.contains(candidate) ||
            libraries.contains(candidate)) {
            continue;
        }
        if (candidate.startsWith("python3-") || candidate.startsWith("python-") ||
            candidate.startsWith("gir1.2-")) {
            if (modules.size() < kModuleLimit) {
                modules << candidate;
            }
        } else if (candidate.startsWith("lib") && libraries.size() < kLibraryLimit) {
            libraries << candidate;
        }
    }

    return modules + libraries;
}

QStringList DependencyResolver::expandLibraryDependencies(const QStringList& packageNames) {
    QStringList expanded;
    if (packageNames.isEmpty()) {
        return expanded;
    }

    QStringList query;
    for (const QString& name : packageNames) {
        const QString cleaned = name.section(' ', 0, 0).trimmed();
        if (!cleaned.isEmpty() && SubprocessWrapper::isSafePackageName(cleaned)) {
            query << cleaned;
        }
    }
    if (query.isEmpty()) {
        return expanded;
    }

    if (RepositoryBrowser::detectPackageManager() != PackageManager::APT) {
        return expanded;   // only Debian's tooling is queried this way
    }

    QStringList arguments = {
        "depends", "--recurse", "--no-recommends", "--no-suggests",
        "--no-conflicts", "--no-breaks", "--no-replaces", "--no-enhances",
        "--implicit"
    };
    arguments << "--";
    arguments << query;

    const ProcessResult result = SubprocessWrapper::execute("apt-cache", arguments, {}, 120000);
    if (!result.success) {
        return expanded;
    }

    expanded = selectRuntimePackages(result.stdoutOutput, query);

    return expanded;
}

QString DependencyResolver::findSystemLibrary(const QString& libName) {
    // Common library search paths
    QStringList searchPaths = {
        "/usr/lib",
        "/usr/lib64",
        "/usr/lib/x86_64-linux-gnu",
        "/usr/lib/aarch64-linux-gnu",
        "/lib",
        "/lib64",
        "/lib/x86_64-linux-gnu"
    };
    
    for (const QString& path : searchPaths) {
        QDir dir(path);
        if (!dir.exists()) continue;
        
        // Search for library files
        QStringList patterns = {
            QString("lib%1.so*").arg(libName),
            QString("%1.so*").arg(libName),
            QString("lib%1-*.so*").arg(libName)
        };
        
        for (const QString& pattern : patterns) {
            QStringList files = dir.entryList({pattern}, QDir::Files);
            if (!files.isEmpty()) {
                return dir.absoluteFilePath(files.first());
            }
        }
    }
    
    return QString();
}

QList<ResolvedDependency> DependencyResolver::resolveDependencies(const QStringList& depends, const QString& outputDir) {
    QList<ResolvedDependency> results;
    m_resolvedLibraries.clear();
    
    if (!m_settings.enabled || depends.isEmpty()) {
        return results;
    }
    
    emit resolveStarted();
    emit log(QString("=== Resolving %1 dependencies ===").arg(depends.size()));
    
    int resolved = 0;
    int failed = 0;
    
    for (int i = 0; i < depends.size(); ++i) {
        const QString& dep = depends[i];
        emit progress(i + 1, depends.size());
        
        ResolvedDependency result;
        result.name = dep;
        
        // Check if it should be excluded
        if (shouldExclude(dep)) {
            emit log(QString("  Skipping system package: %1").arg(dep));
            result.isResolved = true;
            result.resolvedPath = "(system)";
            results.append(result);
            continue;
        }
        
        // Try to find on system first
        QString systemLib = findSystemLibrary(dep);
        if (!systemLib.isEmpty()) {
            emit log(QString("  Found on system: %1 -> %2").arg(dep).arg(systemLib));
            result.isResolved = true;
            result.resolvedPath = systemLib;
            m_resolvedLibraries.append(systemLib);
            resolved++;
            results.append(result);
            continue;
        }
        
        // Download if enabled
        if (m_settings.downloadMissing) {
            emit downloadStarted(dep);
            
            if (downloadAndExtract(dep, outputDir)) {
                result.isResolved = true;
                result.resolvedPath = outputDir;
                resolved++;
                emit downloadCompleted(dep, outputDir);
            } else {
                result.isResolved = false;
                result.error = "Download/extraction failed";
                failed++;
                emit downloadError(dep, result.error);
            }
        } else {
            result.isResolved = false;
            result.error = "Not found on system";
            failed++;
        }
        
        results.append(result);
    }
    
    emit log(QString("=== Dependency resolution complete: %1 resolved, %2 failed ===")
        .arg(resolved).arg(failed));
    emit resolveCompleted(resolved, failed);
    
    return results;
}

bool DependencyResolver::downloadAndExtract(const QString& packageName, const QString& outputDir) {
    emit log(QString("  Downloading: %1").arg(packageName));

    // SEC-HIGH-11: package names originate from untrusted Depends:/Requires:
    // metadata and are passed straight to a package manager, so reject
    // anything that could be parsed as an option before spawning it.
    if (!SubprocessWrapper::isSafePackageName(packageName)) {
        emit log(QString("  Refusing to download package with unsafe name: %1").arg(packageName));
        return false;
    }
    
    // Create temp directory for download
    QString tempDir = SubprocessWrapper::createTemporaryDirectory("appalchemist-deps");
    if (tempDir.isEmpty()) {
        emit log(QString("  Failed to create temporary download directory"));
        return false;
    }
    
    // Use package manager to download
    PackageManager pm = RepositoryBrowser::detectPackageManager();
    ProcessResult result;
    
    switch (pm) {
        case PackageManager::APT:
            result = SubprocessWrapper::execute("apt-get",
                {"download", "--", packageName}, tempDir, 120000);
            break;
        case PackageManager::DNF:
            result = SubprocessWrapper::execute("dnf",
                {"download", "--destdir", tempDir, "--", packageName}, {}, 120000);
            break;
        case PackageManager::PACMAN:
            if (!m_sudoPassword.isEmpty()) {
                // First sync file database if needed
                SubprocessWrapper::executeWithSudo("pacman", {"-Fy"}, m_sudoPassword, {}, 60000);
                // Download package to cache
                result = SubprocessWrapper::executeWithSudo("pacman",
                    {"-Syw", "--noconfirm", "--", packageName}, m_sudoPassword, {}, 120000);
                if (result.success) {
                    // Copy from cache to tempDir (may need sudo)
                    QDir cacheDir("/var/cache/pacman/pkg");
                    QStringList pkgFiles = cacheDir.entryList({QString("%1-*.pkg.tar*").arg(packageName)}, QDir::Files, QDir::Time);
                    if (!pkgFiles.isEmpty()) {
                        QString cachePath = cacheDir.absoluteFilePath(pkgFiles.first());
                        QString destPath = tempDir + "/" + pkgFiles.first();
                        emit log(QString("  Found package in cache: %1").arg(cachePath));
                        // Try direct copy first
                        if (!QFile::copy(cachePath, destPath)) {
                            // If failed, try with sudo
                            emit log(QString("  Direct copy failed, trying with sudo..."));
                            ProcessResult copyResult = SubprocessWrapper::executeWithSudo("cp",
                                {cachePath, destPath}, m_sudoPassword, {}, 30000);
                            if (!copyResult.success) {
                                emit log(QString("  Failed to copy package file: %1").arg(copyResult.stderrOutput));
                                SubprocessWrapper::removeDirectory(tempDir);
                                return false;
                            }
                        }
                        emit log(QString("  Package copied to: %1").arg(destPath));
                    } else {
                        emit log(QString("  Package not found in cache after download"));
                        SubprocessWrapper::removeDirectory(tempDir);
                        return false;
                    }
                } else {
                    emit log(QString("  Failed to download package: %1").arg(result.stderrOutput.left(200)));
                    SubprocessWrapper::removeDirectory(tempDir);
                    return false;
                }
            } else {
                emit log(QString("  Sudo password required for pacman download"));
                SubprocessWrapper::removeDirectory(tempDir);
                return false;
            }
            break;
        default:
            emit log(QString("  Unsupported package manager"));
            SubprocessWrapper::removeDirectory(tempDir);
            return false;
    }
    
    if (!result.success) {
        emit log(QString("  Download failed: %1").arg(result.stderrOutput.left(200)));
        SubprocessWrapper::removeDirectory(tempDir);
        return false;
    }
    
    // Find downloaded package
    QDir dir(tempDir);
    QStringList packages;
    
    if (pm == PackageManager::APT) {
        packages = dir.entryList({"*.deb"}, QDir::Files);
    } else if (pm == PackageManager::DNF) {
        packages = dir.entryList({"*.rpm"}, QDir::Files);
    } else if (pm == PackageManager::PACMAN) {
        packages = dir.entryList({"*.pkg.tar.*", "*.pkg.tar"}, QDir::Files);
    }
    
    if (packages.isEmpty()) {
        emit log(QString("  No package file found after download"));
        SubprocessWrapper::removeDirectory(tempDir);
        return false;
    }
    
    QString packagePath = dir.absoluteFilePath(packages.first());
    emit log(QString("  Downloaded: %1").arg(packagePath));
    
    // Extract libraries
    QStringList libs = extractLibraries(packagePath, outputDir);
    
    if (!libs.isEmpty()) {
        emit log(QString("  Extracted %1 libraries").arg(libs.size()));
        for (const QString& lib : libs) {
            if (QFileInfo::exists(lib)) {
                emit log(QString("    Verified: %1").arg(lib));
            } else {
                emit log(QString("    WARNING: Library not found: %1").arg(lib));
            }
        }
        m_resolvedLibraries.append(libs);
    } else {
        emit log(QString("  WARNING: No libraries extracted from package"));
    }
    
    // Cleanup
    SubprocessWrapper::removeDirectory(tempDir);
    
    return !libs.isEmpty();
}

QStringList DependencyResolver::extractLibraries(const QString& packagePath, const QString& outputDir) {
    QStringList libs;
    
    if (outputDir.trimmed().isEmpty()) {
        qCritical() << "Refusing to extract into empty outputDir";
        emit log("ERROR: Refusing to extract libraries into empty outputDir");
        return libs;
    }

    QString tempExtract = SubprocessWrapper::createTemporaryDirectory("appalchemist-extract");
    if (tempExtract.isEmpty()) {
        emit log("ERROR: Failed to create temporary extraction directory");
        return libs;
    }
    
    bool extracted = false;
    const QString absPackagePath = QFileInfo(packagePath).absoluteFilePath();
    
    if (packagePath.endsWith(".deb")) {
        // Extract .deb
        QString arDir = tempExtract + "/ar";
        QDir().mkpath(arDir);
        
        ProcessResult arResult = SubprocessWrapper::execute("ar", {"x", absPackagePath}, arDir, 30000);
        if (arResult.success) {
            QDir arDirObj(arDir);
            QStringList dataFiles = arDirObj.entryList({"data.tar.*"}, QDir::Files);
            if (!dataFiles.isEmpty()) {
                QString dataTar = arDirObj.absoluteFilePath(dataFiles.first());
                QString dataDir = tempExtract + "/data";
                QDir().mkpath(dataDir);
                
                QStringList tarArgs;
                if (dataTar.endsWith(".gz")) {
                    tarArgs = {"-xzf", dataTar, "-C", dataDir};
                } else if (dataTar.endsWith(".xz")) {
                    tarArgs = {"-xJf", dataTar, "-C", dataDir};
                } else {
                    tarArgs = {"-xf", dataTar, "-C", dataDir};
                }
                
                ProcessResult tarResult = SubprocessWrapper::execute("tar", tarArgs, {}, 60000);
                extracted = tarResult.success;
            }
        }
    } else if (packagePath.endsWith(".rpm")) {
        // Extract .rpm
        QString dataDir = tempExtract + "/data";
        QDir().mkpath(dataDir);
        
        // Use rpm2cpio and cpio via structured pipeline without shell
        ProcessResult result = SubprocessWrapper::executePipeline(
            "rpm2cpio", {absPackagePath},
            "cpio", {"-idm", "--quiet", "--no-absolute-filenames"},
            dataDir,
            60000
        );
        extracted = result.success;
    } else if (packagePath.contains(".pkg.tar")) {
        // Extract Arch Linux .pkg.tar.* (zst, xz, gz, etc.)
        QString dataDir = tempExtract + "/data";
        QDir().mkpath(dataDir);
        
        QStringList tarArgs;
        if (packagePath.endsWith(".zst")) {
            tarArgs = {"--zstd", "-xf", absPackagePath, "-C", dataDir};
        } else if (packagePath.endsWith(".xz")) {
            tarArgs = {"-xJf", absPackagePath, "-C", dataDir};
        } else if (packagePath.endsWith(".gz")) {
            tarArgs = {"-xzf", absPackagePath, "-C", dataDir};
        } else {
            tarArgs = {"-xf", absPackagePath, "-C", dataDir};
        }
        
        ProcessResult result = SubprocessWrapper::execute("tar", tarArgs, {}, 60000);
        extracted = result.success;
    }
    
    if (!extracted) {
        SubprocessWrapper::removeDirectory(tempExtract);
        return libs;
    }
    
    // Find and copy .so files, preserving directory structure
    QString dataDir = tempExtract + "/data";
    QDir dataQDir(dataDir);
    
    // Create lib directory in output
    QString libDir = outputDir + "/usr/lib";
    QDir().mkpath(libDir);
    
    // Recursively find and copy .so files, preserving structure
    std::function<void(const QString&, const QString&)> findLibs = [&](const QString& srcDir, const QString& basePath) {
        QDir d(srcDir);
        for (const QFileInfo& info : d.entryInfoList(QDir::Files)) {
            QString name = info.fileName();
            if (name.contains(".so")) {
                // Calculate relative path from usr/lib
                QString relPath = info.absoluteFilePath();
                if (relPath.contains("/usr/lib/")) {
                    relPath = relPath.mid(relPath.indexOf("/usr/lib/") + 9); // Skip "/usr/lib/"
                } else {
                    relPath = name; // Just filename if not in usr/lib structure
                }
                
                QString dest = libDir + "/" + relPath;
                QFileInfo destInfo(dest);
                QDir().mkpath(destInfo.absolutePath());
                
                if (QFile::copy(info.absoluteFilePath(), dest)) {
                    libs.append(dest);
                    emit log(QString("  Copied library: %1 -> %2").arg(name).arg(relPath));
                    
                    // Create symlinks for .so files using pattern matching (e.g., libgdk-pixbuf-2.0.so.0 -> libgdk-pixbuf-2.0.so.0.4200.8)
                    const QString symlinkDir = destInfo.absolutePath();
                    const int soIdx = name.indexOf(".so.");
                    if (soIdx != -1) {
                        const QString prefix = name.left(soIdx + 3); // "libfoo.so" or "libgdk-pixbuf-2.0.so"
                        const QString versionPart = name.mid(soIdx + 4); // "0.4200.8" or "28.0.0"
                        const QStringList vParts = versionPart.split('.');
                        if (!vParts.isEmpty() && !vParts[0].isEmpty()) {
                            // Major version symlink: libfoo.so.X -> libfoo.so.X.Y.Z
                            const QString symlinkName = prefix + "." + vParts[0];
                            const QString symlinkPath = symlinkDir + "/" + symlinkName;
                            if (symlinkName != name) {
                                if (QFileInfo::exists(symlinkPath) || QFileInfo(symlinkPath).isSymLink()) {
                                    QFile::remove(symlinkPath);
                                }
                                if (QFile::link(name, symlinkPath)) {
                                    emit log(QString("  Created symlink: %1 -> %2").arg(symlinkName).arg(name));
                                }
                            }

                            // Base soname symlink: libfoo.so -> libfoo.so.X
                            const QString baseName = prefix;
                            const QString basePath = symlinkDir + "/" + baseName;
                            if (baseName != name && baseName != symlinkName) {
                                if (QFileInfo::exists(basePath) || QFileInfo(basePath).isSymLink()) {
                                    QFile::remove(basePath);
                                }
                                if (QFile::link(symlinkName, basePath)) {
                                    emit log(QString("  Created symlink: %1 -> %2").arg(baseName).arg(symlinkName));
                                }
                            }
                        }
                    }
                }
            }
        }
        for (const QFileInfo& subdir : d.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            findLibs(subdir.absoluteFilePath(), basePath);
        }
    };
    
    findLibs(dataDir, dataDir);

    // A dependency is more than its shared libraries. Packages routinely rely
    // on a helper script, an interpreter or data files installed by another
    // package, and without them the application fails on its first line. Copy
    // the rest of the payload as well, preserving the layout so the references
    // inside the application keep matching.
    if (m_settings.enabled) {
        if (SubprocessWrapper::copyDirectory(dataDir, outputDir)) {
            emit log("  Bundled the remaining package contents");
        }
    }

    // Cleanup
    SubprocessWrapper::removeDirectory(tempExtract);
    
    return libs;
}

QString DependencyResolver::findPackageForLibrary(const QString& libName) {
    PackageManager pm = RepositoryBrowser::detectPackageManager();
    ProcessResult result;
    
    // Hardcoded mappings for common KDE5 libraries (pacman -F requires synced database)
    static QMap<QString, QString> kf5Mappings = {
        {"libKF5Crash.so", "kcrash5"},
        {"libKF5I18n.so", "ki18n5"},
        {"libKF5ConfigCore.so", "kconfig5"},
        {"libKF5ConfigGui.so", "kconfig5"},
        {"libKF5CoreAddons.so", "kcoreaddons5"},
        {"libKF5GuiAddons.so", "kguiaddons5"},
        {"libKF5WidgetsAddons.so", "kwidgetsaddons5"},
        {"libKF5Completion.so", "kcompletion5"},
        {"libKF5ItemViews.so", "kitemviews5"},
        {"libKF5WindowSystem.so", "kwindowsystem5"},
        {"libKF5Archive.so", "karchive5"},
        {"libKF5Codecs.so", "kcodecs5"},
        {"libKF5Auth.so", "kauth5"},
        {"libKF5Service.so", "kservice5"},
        {"libKF5TextWidgets.so", "ktextwidgets5"},
        {"libKF5XmlGui.so", "kxmlgui5"},
        {"libKF5ItemModels.so", "kitemmodels5"},
        {"libKF5Notifications.so", "knotifications5"},
        {"libKF5JobWidgets.so", "kjobwidgets5"},
        {"libKF5KIO.so", "kio5"},
        {"libKF5Bookmarks.so", "kbookmarks5"},
        {"libgsl.so", "gsl"},
        {"libmlt-7.so", "mlt"},
        {"libmlt++-7.so", "mlt"},
        {"libquazip1-qt5.so", "quazip-qt5"},
        {"libgsl.so", "gsl"},
    };
    
    // Try to match KF5 library by prefix
    for (auto it = kf5Mappings.begin(); it != kf5Mappings.end(); ++it) {
        if (libName.startsWith(it.key())) {
            emit log(QString("  Using hardcoded mapping: %1 -> %2").arg(libName).arg(it.value()));
            return it.value();
        }
    }
    
    switch (pm) {
        case PackageManager::PACMAN: {
            // Use pacman -F to find which package provides the library
            result = SubprocessWrapper::execute("pacman", {"-F", libName}, {}, 30000);
            if (result.success && !result.stdoutOutput.isEmpty()) {
                // Parse output: "extra/kcrash 5.xxx.x [installed]\n    /usr/lib/libKF5Crash.so.5"
                QStringList lines = result.stdoutOutput.split('\n');
                for (const QString& line : lines) {
                    QString trimmed = line.trimmed();
                    if (!trimmed.isEmpty() && !trimmed.startsWith("/") && trimmed.contains('/')) {
                        // Format: repo/package version
                        QString pkgName = trimmed.split(' ').first();
                        if (pkgName.contains('/')) {
                            pkgName = pkgName.split('/').last();
                        }
                        emit log(QString("  Found package for %1: %2").arg(libName).arg(pkgName));
                        return pkgName;
                    }
                }
            }
            break;
        }
        case PackageManager::APT: {
            // Use apt-file search
            result = SubprocessWrapper::execute("apt-file", {"search", libName}, {}, 30000);
            if (result.success && !result.stdoutOutput.isEmpty()) {
                // Parse: package: /path/to/lib
                QStringList lines = result.stdoutOutput.split('\n');
                if (!lines.isEmpty()) {
                    QString firstLine = lines.first().trimmed();
                    if (firstLine.contains(':')) {
                        QString pkgName = firstLine.split(':').first().trimmed();
                        emit log(QString("  Found package for %1: %2").arg(libName).arg(pkgName));
                        return pkgName;
                    }
                }
            }
            break;
        }
        case PackageManager::DNF: {
            // Use dnf provides
            result = SubprocessWrapper::execute("dnf", {"provides", "*/" + libName}, {}, 30000);
            if (result.success && !result.stdoutOutput.isEmpty()) {
                QStringList lines = result.stdoutOutput.split('\n');
                for (const QString& line : lines) {
                    if (line.contains("-") && !line.startsWith(" ")) {
                        QString pkgName = line.split('-').first().trimmed();
                        if (!pkgName.isEmpty()) {
                            emit log(QString("  Found package for %1: %2").arg(libName).arg(pkgName));
                            return pkgName;
                        }
                    }
                }
            }
            break;
        }
        default:
            break;
    }
    
    return QString();
}

QStringList DependencyResolver::findMissingLibraries(const QString& binaryPath) {
    QStringList missing;
    
    // Run safe ldd on the binary
    QStringList lines = runSafeLdd(binaryPath);
    if (lines.isEmpty()) {
        emit log(QString("  Failed to run ldd on %1").arg(binaryPath));
        return missing;
    }
    
    // Parse ldd output for "not found" lines
    for (const QString& line : lines) {
        if (line.contains("not found")) {
            // Format: "libXXX.so.N => not found"
            QString trimmed = line.trimmed();
            int arrowPos = trimmed.indexOf("=>");
            if (arrowPos > 0) {
                QString libName = trimmed.left(arrowPos).trimmed();
                if (!libName.isEmpty() && !missing.contains(libName)) {
                    missing.append(libName);
                }
            }
        }
    }
    
    return missing;
}

bool DependencyResolver::resolveMissingLibraries(const QString& binaryPath, const QString& appDir) {
    emit log(QString("=== Analyzing missing libraries for %1 ===").arg(QFileInfo(binaryPath).fileName()));
    
    // Set LD_LIBRARY_PATH to include AppDir libs for proper analysis
    QString libPath = appDir + "/usr/lib";
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    QString existingLdPath = env.value("LD_LIBRARY_PATH");
    env.insert("LD_LIBRARY_PATH", libPath + ":" + existingLdPath);
    
    // Run safe ldd with AppDir libs
    QStringList lines = runSafeLdd(binaryPath, env);
    if (lines.isEmpty()) {
        emit log(QString("  Failed to analyze binary"));
        return false;
    }
    
    // Parse missing libraries
    QStringList missing;
    for (const QString& line : lines) {
        if (line.contains("not found")) {
            QString trimmed = line.trimmed();
            int arrowPos = trimmed.indexOf("=>");
            if (arrowPos > 0) {
                QString libName = trimmed.left(arrowPos).trimmed();
                if (!libName.isEmpty() && !missing.contains(libName)) {
                    missing.append(libName);
                }
            }
        }
    }
    
    if (missing.isEmpty()) {
        emit log("  No missing libraries found");
        return true;
    }
    
    emit log(QString("  Found %1 missing libraries").arg(missing.size()));
    
    // Deduplicate by finding packages
    QSet<QString> packagesToDownload;
    for (const QString& lib : missing) {
        if (shouldExclude(lib)) {
            emit log(QString("  Skipping system library: %1").arg(lib));
            continue;
        }
        
        QString pkgName = findPackageForLibrary(lib);
        if (!pkgName.isEmpty()) {
            packagesToDownload.insert(pkgName);
        } else {
            emit log(QString("  WARNING: No package found for %1").arg(lib));
        }
    }
    
    emit log(QString("  Need to download %1 packages").arg(packagesToDownload.size()));
    
    // Download and extract each package
    int downloaded = 0;
    for (const QString& pkg : packagesToDownload) {
        emit log(QString("  Downloading package: %1").arg(pkg));
        if (downloadAndExtract(pkg, appDir)) {
            downloaded++;
        }
    }
    
    emit log(QString("=== Downloaded %1/%2 packages ===").arg(downloaded).arg(packagesToDownload.size()));
    
    // Verify libraries are now available
    if (downloaded > 0) {
        emit log("=== Verifying downloaded libraries ===");
        QStringList verifyLines = runSafeLdd(binaryPath, env);
        if (!verifyLines.isEmpty()) {
            int stillMissing = 0;
            for (const QString& line : verifyLines) {
                if (line.contains("not found")) {
                    stillMissing++;
                    QString trimmed = line.trimmed();
                    int arrowPos = trimmed.indexOf("=>");
                    if (arrowPos > 0) {
                        QString libName = trimmed.left(arrowPos).trimmed();
                        emit log(QString("  Still missing: %1").arg(libName));
                    }
                }
            }
            if (stillMissing == 0) {
                emit log("  All libraries resolved successfully!");
            } else {
                emit log(QString("  WARNING: %1 libraries still missing").arg(stillMissing));
            }
        }
    }
    
    return downloaded > 0;
}

