#ifndef DEPENDENCY_RESOLVER_H
#define DEPENDENCY_RESOLVER_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QSet>
#include "repository_browser.h"

// Dependency resolution settings
struct DependencySettings {
    // Copy every shared library the AppDir binaries link against into the
    // AppDir itself. This is what makes the produced AppImage self-contained,
    // so it is on by default and should only be turned off for debugging.
    bool bundleSystemLibraries = true;
    // Additionally query the host package manager for libraries that are not
    // installed locally. Needs network/sudo, so it stays opt-in.
    bool enabled = false;
    bool downloadMissing = true;           // Download missing libraries
    bool includeRecommended = false;       // Include recommended packages
    bool excludeSystemLibs = true;         // Skip glibc, libpthread, etc.
    QStringList excludePatterns;           // Additional exclude patterns
};

// Result of bundling host libraries into an AppDir.
struct LibraryBundleReport {
    bool ran = false;                   // Whether bundling was attempted at all
    bool patchelfAvailable = false;     // Whether RPATHs could be rewritten
    int scannedBinaries = 0;            // ELF objects inspected
    int skippedSystemLibraries = 0;     // Deliberately left to the host
    QStringList bundled;                // Sonames copied into the AppDir
    QStringList unresolved;             // Sonames ldd could not resolve

    QString summary() const;
};

// Resolved dependency information
struct ResolvedDependency {
    QString name;               // Package/library name
    QString version;            // Version constraint (if any)
    QString resolvedPath;       // Path to downloaded/found library
    bool isOptional = false;    // Optional dependency
    bool isResolved = false;    // Whether it was successfully resolved
    QString error;              // Error message if not resolved
};

class DependencyResolver : public QObject {
    Q_OBJECT

public:
    explicit DependencyResolver(QObject* parent = nullptr);
    ~DependencyResolver();
    
    // Set resolution settings
    void setSettings(const DependencySettings& settings);
    DependencySettings settings() const { return m_settings; }
    
    // Parse dependencies from package control file
    QStringList parseDependencies(const QString& controlFilePath);
    
    // Parse dependencies from Depends: string
    QStringList parseDependsString(const QString& dependsStr);
    
    // Resolve dependencies - download missing ones
    QList<ResolvedDependency> resolveDependencies(const QStringList& depends, const QString& outputDir);
    
    // Check if a library/package should be excluded
    bool shouldExclude(const QString& name);

    // Check if a concrete soname must stay a host dependency (glibc, GL, ...)
    bool shouldExcludeSoname(const QString& soname) const;

    // Copy every resolvable shared-library dependency of every ELF object in
    // the AppDir into the AppDir, transitively, and point RPATHs at it.
    LibraryBundleReport bundleSystemLibraries(const QString& appDirPath);
    
    // Get libraries to include in AppDir
    QStringList getResolvedLibraries() const { return m_resolvedLibraries; }
    
    // Set sudo password for pacman operations
    void setSudoPassword(const QString& password) { m_sudoPassword = password; }

signals:
    void log(const QString& message);
    void progress(int current, int total);
    void resolveStarted();
    void resolveCompleted(int resolved, int failed);
    void downloadStarted(const QString& package);
    void downloadCompleted(const QString& package, const QString& path);
    void downloadError(const QString& package, const QString& error);

private:
    DependencySettings m_settings;
    QStringList m_resolvedLibraries;
    QSet<QString> m_excludePatterns;
    QSet<QString> m_sonameExcludePatterns;
    RepositoryBrowser* m_browser;
    QString m_sudoPassword;
    
    void initializeExcludePatterns();
    void initializeSonameExcludePatterns();

    // Copies the loadable-module trees (gdk-pixbuf loaders, GIO modules, GTK
    // input methods, GSettings schemas) that belong to libraries we just
    // bundled. Without them a bundled GTK/GIO stack fails at startup because it
    // looks for its modules under the host prefix. Newly copied ELF modules are
    // appended to \a newElfObjects so their own dependencies get bundled too.
    void bundleRuntimeModules(const QString& appDirPath,
                              const QStringList& bundledSonames,
                              const QStringList& hostLibraryDirs,
                              QStringList& newElfObjects);
    
    // Check if library is available on the system
    QString findSystemLibrary(const QString& libName);
    
    // Download a package and extract libraries
    bool downloadAndExtract(const QString& packageName, const QString& outputDir);
    
    // Extract libraries from downloaded package
    QStringList extractLibraries(const QString& packagePath, const QString& outputDir);
    
    // Parse version constraint from dependency string
    void parseVersionConstraint(const QString& dep, QString& name, QString& version, QString& op);
    
    // Find which package provides a library (using pacman -F, apt-file, etc.)
    QString findPackageForLibrary(const QString& libName);
    
    // Analyze binary with ldd and find missing libraries
    QStringList findMissingLibraries(const QString& binaryPath);
    
public:
    // Resolve missing libraries by downloading their packages
    bool resolveMissingLibraries(const QString& binaryPath, const QString& appDir);
};

#endif // DEPENDENCY_RESOLVER_H


