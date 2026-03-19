#ifndef SIZE_OPTIMIZER_H
#define SIZE_OPTIMIZER_H

#include <QObject>
#include <QString>
#include <QStringList>

// Compression level for AppImage
enum class CompressionLevel {
    FAST,       // gzip -1 (fastest, largest)
    NORMAL,     // gzip -6 (default)
    MAXIMUM,    // zstd -19 (slowest, smallest)
    ULTRA       // zstd --ultra -22 (extreme compression)
};

// Size optimization settings
struct OptimizationSettings {
    bool enabled = false;
    bool stripBinaries = true;              // Strip debug symbols from ELF binaries
    bool removeStaticLibs = true;           // Remove *.a files
    bool removeLiToolLibs = true;           // Remove *.la files
    bool removeDocumentation = true;        // Remove man/, doc/ directories
    bool removeUnneededLocales = true;      // Remove locale/ except system language
    bool removeHeaderFiles = true;          // Remove *.h, *.hpp files
    bool removePkgConfig = true;            // Remove *.pc files
    QStringList keepLocales = {"en", "en_US"};  // Locales to keep
    CompressionLevel compression = CompressionLevel::NORMAL;
};

// Size report structure
struct SizeReport {
    qint64 originalSize = 0;
    qint64 optimizedSize = 0;
    qint64 strippedBytes = 0;
    qint64 removedFiles = 0;
    qint64 removedBytes = 0;
    QStringList removedPaths;
    
    double reductionPercent() const {
        if (originalSize == 0) return 0;
        return (1.0 - (double)optimizedSize / originalSize) * 100.0;
    }
    
    QString summary() const {
        return QString("Original: %1 MB → Optimized: %2 MB (-%3%)")
            .arg(originalSize / 1024.0 / 1024.0, 0, 'f', 1)
            .arg(optimizedSize / 1024.0 / 1024.0, 0, 'f', 1)
            .arg(reductionPercent(), 0, 'f', 1);
    }
};

class SizeOptimizer : public QObject {
    Q_OBJECT

public:
    explicit SizeOptimizer(QObject* parent = nullptr);
    ~SizeOptimizer();
    
    // Set optimization settings
    void setSettings(const OptimizationSettings& settings);
    OptimizationSettings settings() const { return m_settings; }
    
    // Optimize AppDir
    bool optimizeAppDir(const QString& appDirPath);
    
    // Get size report after optimization
    SizeReport report() const { return m_report; }
    
    // Get compression arguments for appimagetool
    QStringList getCompressionArgs() const;
    
    // Calculate directory size
    static qint64 calculateDirSize(const QString& dirPath);

signals:
    void log(const QString& message);
    void progress(int percentage, const QString& message);

private:
    OptimizationSettings m_settings;
    SizeReport m_report;
    
    // Individual optimization steps
    bool stripBinaries(const QString& appDirPath);
    bool removeStaticLibraries(const QString& appDirPath);
    bool removeLiToolFiles(const QString& appDirPath);
    bool removeDocumentation(const QString& appDirPath);
    bool removeUnneededLocales(const QString& appDirPath);
    bool removeHeaderFiles(const QString& appDirPath);
    bool removePkgConfigFiles(const QString& appDirPath);
    
    // Helper methods
    bool isElfBinary(const QString& filePath);
    bool stripFile(const QString& filePath);
    qint64 removeMatchingFiles(const QString& dirPath, const QStringList& patterns);
    qint64 removeDirectories(const QString& basePath, const QStringList& dirNames);
    bool shouldKeepLocale(const QString& localeName);
};

#endif // SIZE_OPTIMIZER_H


