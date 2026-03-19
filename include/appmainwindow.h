#ifndef APPMAINWINDOW_H
#define APPMAINWINDOW_H

#include <QMainWindow>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTextEdit>
#include <QProgressBar>
#include <QFileDialog>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QThread>
#include <QPropertyAnimation>
#include <QEasingCurve>
#include <QGraphicsDropShadowEffect>
#include <QTimer>
#include <QGraphicsEffect>
#include <QCheckBox>
#include <QComboBox>
#include "conversion_controller.h"
#include "size_optimizer.h"
#include "dependency_resolver.h"
#include "search_dialog.h"

class AppMainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit AppMainWindow(QWidget* parent = nullptr);
    ~AppMainWindow();

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;

private slots:
    void onBuildClicked();
    void onSelectOutputDirClicked();
    void onSearchClicked();
    void onPackageSelected(const QString& packagePath);
    void onPackageStarted(int index, int totalCount, const QString& packagePath);
    void onProgress(int percentage, const QString& message);
    void onLog(const QString& message);
    void onError(const QString& errorMessage);
    void onSuccess(const QString& appImagePath);
    void onConversionFinished(int successCount, int failureCount, bool cancelled);
    void onSudoPasswordRequested(const QString& packagePath, const QString& reason);
    void onOpenOutputDirClicked();

private:
    void setupUI();
    void setPackageFile(const QString& filePath);
    void setPackageFiles(const QStringList& filePaths);  // For batch
    void resetUI();
    void enableControls(bool enabled);
    QString formatLogMessage(const QString& message);
    
    // Animation methods
    void animateDropAreaDragEnter();
    void animateDropAreaDragLeave();
    void animateDropAreaDrop();
    void animateOutputDirSelection();
    void animateProgressBar(int targetValue);
    void animateSuccess();
    void animateButtonPulse(QPushButton* button);
    
    // UI Components
    QWidget* m_centralWidget;
    QVBoxLayout* m_mainLayout;
    
    // Drag and drop area
    QLabel* m_dropArea;
    QLabel* m_fileLabel;
    
    // Controls
    QPushButton* m_buildButton;
    QPushButton* m_selectOutputDirButton;
    QPushButton* m_openOutputDirButton;
    QPushButton* m_searchButton;
    QLabel* m_outputDirLabel;
    
    // Progress
    QProgressBar* m_progressBar;
    QLabel* m_statusLabel;
    
    // Log
    QTextEdit* m_logText;
    
    // Conversion
    ConversionController* m_conversionController;
    
    // Optimization controls
    QCheckBox* m_optimizeCheckBox;
    QCheckBox* m_resolveDepsCheckBox;
    QComboBox* m_compressionComboBox;
    
    // State
    QString m_packageFilePath;
    QStringList m_packageFilePaths;  // For batch conversion
    QString m_outputDir;
    bool m_isProcessing;
    qint64 m_conversionStartTime;
    
    // Animations
    QPropertyAnimation* m_dropAreaAnimation;
    QPropertyAnimation* m_progressAnimation;
    QPropertyAnimation* m_successAnimation;
    QPropertyAnimation* m_buttonPulseAnimation;
    QGraphicsDropShadowEffect* m_dropAreaShadow;
    QTimer* m_pulseTimer;
    int m_currentProgress;
};

#endif // APPMAINWINDOW_H
