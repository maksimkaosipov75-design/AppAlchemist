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
#include "packagetoappimagepipeline.h"

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
    void onProgress(int percentage, const QString& message);
    void onLog(const QString& message);
    void onError(const QString& errorMessage);
    void onSuccess(const QString& appImagePath);
    void onPipelineFinished();
    void onOpenOutputDirClicked();

private:
    void setupUI();
    void setPackageFile(const QString& filePath);
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
    QLabel* m_outputDirLabel;
    
    // Progress
    QProgressBar* m_progressBar;
    QLabel* m_statusLabel;
    
    // Log
    QTextEdit* m_logText;
    
    // Pipeline
    PackageToAppImagePipeline* m_pipeline;
    QThread* m_pipelineThread;
    
    // State
    QString m_packageFilePath;
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

