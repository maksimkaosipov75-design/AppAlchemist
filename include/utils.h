#ifndef UTILS_H
#define UTILS_H

#include <QString>
#include <QStringList>
#include <QProcess>
#include <QByteArray>
#include <memory>

struct ProcessResult {
    bool success;
    int exitCode;
    QString stdoutOutput;
    QString stderrOutput;
    QString errorMessage;
};

class SubprocessWrapper {
public:
    static ProcessResult execute(const QString& command, 
                                 const QStringList& arguments = {},
                                 const QString& workingDirectory = {},
                                 int timeoutMs = 30000,
                                 const QProcessEnvironment& environment = QProcessEnvironment());
    
    // Runs `producer args | consumer args` without going through a shell,
    // so paths with quotes/spaces/special characters are passed safely.
    static ProcessResult executePipeline(const QString& producerCommand,
                                         const QStringList& producerArguments,
                                         const QString& consumerCommand,
                                         const QStringList& consumerArguments,
                                         const QString& workingDirectory = {},
                                         int timeoutMs = 30000);

    static ProcessResult executeWithSudo(const QString& command,
                                         const QStringList& arguments = {},
                                         const QString& password = {},
                                         const QString& workingDirectory = {},
                                         int timeoutMs = 30000);
    
    static bool copyFile(const QString& source, const QString& destination);
    static bool copyFile(const QString& source, const QString& destination,
                         const QString& extractedRoot, const QString& destRoot);
    static bool copyDirectory(const QString& source, const QString& destination);
    static bool copyDirectory(const QString& source, const QString& destination,
                              const QString& extractedRoot, const QString& destRoot);
    static bool createDirectory(const QString& path);
    static bool removeDirectory(const QString& path);
    static QString generateHash(const QString& filePath);
    static bool setExecutable(const QString& filePath);
    static bool createHardLink(const QString& source, const QString& destination);
};

// Architecture detection utility
QString detectSystemArchitecture();
QString extractDesktopExecBinary(const QString& execCommand);
QString resolveExecutableFromCommand(const QString& execCommand, const QStringList& executables);

// Normalize raw RPM RPMTAG_REQUIRENAME / `rpm -qpR` entries into resolvable
// dependency tokens: strips version constraints and capability suffixes so
// sonames survive ("libc.so.6()(64bit)" -> "libc.so.6"), drops synthetic
// requirements (rpmlib()/config()/rtld()/pkgconfig()/file paths), and
// de-duplicates. Exposed here so it can be unit-tested in isolation.
QStringList normalizeRpmRequires(const QStringList& rawRequires);

#endif // UTILS_H
