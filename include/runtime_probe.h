#ifndef RUNTIME_PROBE_H
#define RUNTIME_PROBE_H

#include "debparser.h"
#include "package_profile.h"
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

struct RuntimeProbeResult {
    bool success = false;
    bool syntaxCheckPassed = false;
    bool fileChecksPassed = true;
    bool commandExecuted = false;
    bool recognizedHealthyOutput = false;
    bool matchedFailurePattern = false;
    int exitCode = -1;
    QString profileName;
    QString program;
    QString workingDirectory;
    QStringList arguments;
    QStringList checks;
    QStringList warnings;
    QStringList failures;
    QString stdoutOutput;
    QString stderrOutput;

    QString commandSummary() const;
    QString summary() const;
};

class RuntimeProbePolicy {
public:
    static RuntimeProbeResult probe(const QString& appDirPath,
                                    const PackageProfile& profile,
                                    const PackageMetadata& metadata,
                                    const QString& primaryExecutable);
};

#endif // RUNTIME_PROBE_H
