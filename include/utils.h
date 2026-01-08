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
    
    static bool copyFile(const QString& source, const QString& destination);
    static bool copyDirectory(const QString& source, const QString& destination);
    static bool createDirectory(const QString& path);
    static bool removeDirectory(const QString& path);
    static QString generateHash(const QString& filePath);
    static bool setExecutable(const QString& filePath);
    static bool createHardLink(const QString& source, const QString& destination);
};

#endif // UTILS_H

