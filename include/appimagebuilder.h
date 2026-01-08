#ifndef APPIMAGEBUILDER_H
#define APPIMAGEBUILDER_H

#include <QObject>
#include <QString>

class AppImageBuilder : public QObject {
    Q_OBJECT

public:
    explicit AppImageBuilder(QObject* parent = nullptr);
    
    bool buildAppImage(const QString& appDirPath, const QString& outputPath, bool compress = true);
    static QString findAppImageTool();
    static void preloadAppImageTool();
    static bool downloadAppImageTool();
    static QString getBundledAppImageToolPath();

signals:
    void log(const QString& message);

private:
    QString m_appImageToolPath;
    static bool checkAppImageTool(const QString& path);
    static QString getCachedAppImageToolPath();
    static bool cacheAppImageTool(const QString& sourcePath);
};

#endif // APPIMAGEBUILDER_H
