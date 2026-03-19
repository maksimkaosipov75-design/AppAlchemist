#ifndef CLI_CONVERTER_H
#define CLI_CONVERTER_H

#include <QObject>
#include <QString>
#include <QThread>
#include <QFile>
#include <QTextStream>
#include <QStandardPaths>
#include <QDir>
#include "packagetoappimagepipeline.h"
#include "cache_manager.h"
#include "utils.h"

class CliConverter : public QObject {
    Q_OBJECT

public:
    explicit CliConverter(QObject* parent = nullptr);
    ~CliConverter();
    
    // Convert package to AppImage
    // Returns exit code: 0 on success, 1 on error
    int convert(const QString& packagePath, const QString& outputDir = QString(), bool autoLaunch = true);
    
    // Batch convert multiple packages
    // Returns exit code: 0 if all succeeded, 1 if any failed
    int convertBatch(const QStringList& packagePaths, const QString& outputDir = QString(), bool autoLaunch = false);
    
    // Send system notification
    static void sendNotification(const QString& title, const QString& message, const QString& urgency = "normal");

private slots:
    void onProgress(int percentage, const QString& message);
    void onLog(const QString& message);
    void onError(const QString& errorMessage);
    void onSuccess(const QString& appImagePath);
    void onPipelineFinished();

private:
    void setupLogging();
    void logToFile(const QString& message);
    bool launchAppImage(const QString& appImagePath);
    void createDesktopEntry(const QString& appImagePath);
    QString extractAndInstallIcon(const QDir& squashfsRoot, const QString& appImagePath, const QString& desktopContent,
                                  const QString& desktopBaseName = QString());
    QString determineAppImagePath(const QString& packagePath, const QString& customOutputDir);
    
    PackageToAppImagePipeline* m_pipeline;
    QThread* m_pipelineThread;
    QFile* m_logFile;
    QTextStream* m_logStream;
    QString m_packagePath;
    QString m_outputDir;
    bool m_autoLaunch;
    bool m_success;
    QString m_resultAppImagePath;
    qint64 m_conversionStartTime;
};

#endif // CLI_CONVERTER_H
