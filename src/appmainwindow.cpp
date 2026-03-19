#include "appmainwindow.h"
#include "tarballparser.h"
#include "search_dialog.h"
#include "appstore_window.h"
// #include "store/store_window.h" // Removed - not used
#include "repository_browser.h"
#include <QApplication>
#include <QMessageBox>
#include <QInputDialog>
#include <QDir>
#include <QFileInfo>
#include <QDesktopServices>
#include <QUrl>
#include <QStyle>
#include <QPalette>
#include <QTextCursor>
#include <QMouseEvent>
#include <QSizePolicy>
#include <QGraphicsOpacityEffect>
#include <QDragLeaveEvent>
#include <QRegularExpression>
#include <QMenuBar>
#include <QIcon>

AppMainWindow::AppMainWindow(QWidget* parent)
    : QMainWindow(parent)
    , m_isProcessing(false)
    , m_conversionController(new ConversionController(this))
    , m_dropAreaAnimation(nullptr)
    , m_progressAnimation(nullptr)
    , m_successAnimation(nullptr)
    , m_buttonPulseAnimation(nullptr)
    , m_dropAreaShadow(nullptr)
    , m_pulseTimer(nullptr)
    , m_currentProgress(0)
{
    setWindowTitle("AppAlchemist - Package to AppImage Converter");
    setMinimumSize(900, 700);
    resize(1000, 800);
    
    // Remove window icon first (may be the button in top left corner)
    setWindowIcon(QIcon());
    
    // Remove menu bar completely (hides the button in top left corner)
    setMenuBar(nullptr);
    
    // Remove system menu button in top left corner - use explicit flags WITHOUT system menu
    Qt::WindowFlags flags = Qt::Window | 
                            Qt::WindowTitleHint | 
                            Qt::WindowMinimizeButtonHint | 
                            Qt::WindowMaximizeButtonHint | 
                            Qt::WindowCloseButtonHint;
    setWindowFlags(flags);
    
    setupUI();
    resetUI();
    
    // Initialize animations
    m_dropAreaAnimation = new QPropertyAnimation(this);
    m_progressAnimation = new QPropertyAnimation(m_progressBar, "value", this);
    m_progressAnimation->setDuration(300);
    m_progressAnimation->setEasingCurve(QEasingCurve::OutCubic);
    
    m_successAnimation = new QPropertyAnimation(this);
    m_buttonPulseAnimation = new QPropertyAnimation(this);
    
    m_pulseTimer = new QTimer(this);
    m_pulseTimer->setInterval(50);

    connect(m_conversionController, &ConversionController::packageStarted,
            this, &AppMainWindow::onPackageStarted);
    connect(m_conversionController, &ConversionController::progress,
            this, &AppMainWindow::onProgress);
    connect(m_conversionController, &ConversionController::log,
            this, &AppMainWindow::onLog);
    connect(m_conversionController, &ConversionController::error,
            this, &AppMainWindow::onError);
    connect(m_conversionController, &ConversionController::success,
            this, &AppMainWindow::onSuccess);
    connect(m_conversionController, &ConversionController::finished,
            this, &AppMainWindow::onConversionFinished);
    connect(m_conversionController, &ConversionController::sudoPasswordRequested,
            this, &AppMainWindow::onSudoPasswordRequested);
}

AppMainWindow::~AppMainWindow() {
}

void AppMainWindow::setupUI() {
    // Apply dark theme with purple-blue gradient background
    m_centralWidget = new QWidget(this);
    setCentralWidget(m_centralWidget);
    
    // Set main window background with gradient
    m_centralWidget->setStyleSheet(
        "QWidget {"
        "background: qlineargradient(x1:0, y1:0, x2:0, y2:1, "
        "stop:0 #1a1a2e, stop:0.5 #16213e, stop:1 #0f3460);"
        "color: #e0e0e0;"
        "}"
    );
    
    m_mainLayout = new QVBoxLayout(m_centralWidget);
    m_mainLayout->setSpacing(15);
    m_mainLayout->setContentsMargins(30, 30, 30, 30);
    m_mainLayout->setSizeConstraint(QLayout::SetMinimumSize);
    
    // Title with gradient text effect
    QLabel* titleLabel = new QLabel("AppAlchemist", this);
    QFont titleFont = titleLabel->font();
    titleFont.setPointSize(28);
    titleFont.setBold(true);
    titleFont.setLetterSpacing(QFont::PercentageSpacing, 110);
    titleLabel->setFont(titleFont);
    titleLabel->setAlignment(Qt::AlignCenter);
    titleLabel->setStyleSheet(
        "QLabel {"
        "color: #ffffff;"
        "background: transparent;"
        "padding: 5px;"
        "}"
    );
    m_mainLayout->addWidget(titleLabel);
    
    // Subtitle
    QLabel* subtitleLabel = new QLabel("Package to AppImage Converter", this);
    QFont subtitleFont = subtitleLabel->font();
    subtitleFont.setPointSize(11);
    subtitleFont.setItalic(true);
    subtitleLabel->setFont(subtitleFont);
    subtitleLabel->setAlignment(Qt::AlignCenter);
    subtitleLabel->setStyleSheet("color: #b0b0b0; background: transparent; padding-bottom: 5px;");
    m_mainLayout->addWidget(subtitleLabel);
    
    // Drag and drop area with modern design
    m_dropArea = new QLabel(this);
    m_dropArea->setMinimumHeight(120);
    m_dropArea->setMaximumHeight(160);
    m_dropArea->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    
    // Add shadow effect for depth
    m_dropAreaShadow = new QGraphicsDropShadowEffect(this);
    m_dropAreaShadow->setBlurRadius(15);
    m_dropAreaShadow->setColor(QColor(108, 92, 231, 100));
    m_dropAreaShadow->setOffset(0, 3);
    m_dropArea->setGraphicsEffect(m_dropAreaShadow);
    
    m_dropArea->setStyleSheet(
        "QLabel {"
        "border: 3px dashed #6c5ce7;"
        "border-radius: 15px;"
        "background: qlineargradient(x1:0, y1:0, x2:1, y2:1, "
        "stop:0 rgba(108, 92, 231, 0.1), stop:1 rgba(74, 144, 226, 0.1));"
        "padding: 30px;"
        "color: #d0d0d0;"
        "font-size: 14px;"
        "}"
        "QLabel:hover {"
        "border-color: #a29bfe;"
        "background: qlineargradient(x1:0, y1:0, x2:1, y2:1, "
        "stop:0 rgba(108, 92, 231, 0.2), stop:1 rgba(74, 144, 226, 0.2));"
        "}"
    );
    m_dropArea->setAlignment(Qt::AlignCenter);
    m_dropArea->setAcceptDrops(true);
    m_dropArea->setText("Drag and drop a package file here\n(.deb, .rpm, .tar.gz, .tar.xz, .zip)\nor click to select a file");
    m_dropArea->setWordWrap(true);
    m_dropArea->installEventFilter(this);
    m_mainLayout->addWidget(m_dropArea);
    
    // Search packages button
    m_searchButton = new QPushButton("🔍 Search Packages in Repository", this);
    m_searchButton->setStyleSheet(
        "QPushButton {"
        "background-color: rgba(74, 144, 226, 0.4);"
        "color: white;"
        "font-weight: 500;"
        "border-radius: 8px;"
        "padding: 10px 20px;"
        "border: 1px solid rgba(74, 144, 226, 0.6);"
        "}"
        "QPushButton:hover {"
        "background-color: rgba(74, 144, 226, 0.6);"
        "border-color: rgba(74, 144, 226, 0.8);"
        "}"
        "QPushButton:pressed {"
        "background-color: rgba(74, 144, 226, 0.8);"
        "}"
    );
    m_searchButton->setToolTip("Search and download packages from system repositories (apt/dnf/pacman)");
    connect(m_searchButton, &QPushButton::clicked, this, &AppMainWindow::onSearchClicked);
    m_mainLayout->addWidget(m_searchButton);
    
    // File label with modern styling
    m_fileLabel = new QLabel("No file selected", this);
    m_fileLabel->setAlignment(Qt::AlignCenter);
    m_fileLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_fileLabel->setStyleSheet(
        "QLabel {"
        "color: #a0a0a0;"
        "font-style: italic;"
        "font-size: 12px;"
        "padding: 5px;"
        "background: transparent;"
        "}"
    );
    m_mainLayout->addWidget(m_fileLabel);
    
    // Add spacer to prevent overlap
    m_mainLayout->addSpacing(5);
    
    // Output directory selection with modern card design
    QHBoxLayout* outputLayout = new QHBoxLayout();
    outputLayout->setSpacing(10);
    QLabel* outputLabel = new QLabel("Output directory:", this);
    outputLabel->setStyleSheet("color: #e0e0e0; font-weight: 500; background: transparent;");
    m_outputDirLabel = new QLabel(QDir::homePath(), this);
    m_outputDirLabel->setStyleSheet(
        "QLabel {"
        "padding: 8px 12px;"
        "background-color: rgba(255, 255, 255, 0.1);"
        "border: 1px solid rgba(255, 255, 255, 0.2);"
        "border-radius: 8px;"
        "color: #ffffff;"
        "}"
    );
    m_outputDirLabel->setMinimumHeight(35);
    m_outputDirLabel->setWordWrap(true);
    m_selectOutputDirButton = new QPushButton("Browse...", this);
    m_selectOutputDirButton->setStyleSheet(
        "QPushButton {"
        "background-color: rgba(108, 92, 231, 0.6);"
        "color: white;"
        "font-weight: 500;"
        "border-radius: 8px;"
        "padding: 8px 20px;"
        "border: none;"
        "}"
        "QPushButton:hover {"
        "background-color: rgba(108, 92, 231, 0.8);"
        "}"
        "QPushButton:pressed {"
        "background-color: rgba(108, 92, 231, 1.0);"
        "}"
    );
    connect(m_selectOutputDirButton, &QPushButton::clicked, this, &AppMainWindow::onSelectOutputDirClicked);
    outputLayout->addWidget(outputLabel);
    outputLayout->addWidget(m_outputDirLabel, 1);
    outputLayout->addWidget(m_selectOutputDirButton);
    m_mainLayout->addLayout(outputLayout);
    
    // Optimization options
    QHBoxLayout* optimizeLayout = new QHBoxLayout();
    optimizeLayout->setSpacing(15);
    
    m_optimizeCheckBox = new QCheckBox("Optimize size", this);
    m_optimizeCheckBox->setStyleSheet(
        "QCheckBox {"
        "color: #e0e0e0;"
        "font-weight: 500;"
        "background: transparent;"
        "}"
        "QCheckBox::indicator {"
        "width: 18px;"
        "height: 18px;"
        "border: 2px solid rgba(108, 92, 231, 0.6);"
        "border-radius: 4px;"
        "background-color: rgba(0, 0, 0, 0.2);"
        "}"
        "QCheckBox::indicator:checked {"
        "background-color: rgba(108, 92, 231, 0.8);"
        "border-color: #6c5ce7;"
        "}"
        "QCheckBox::indicator:hover {"
        "border-color: #a29bfe;"
        "}"
    );
    m_optimizeCheckBox->setToolTip("Remove unnecessary files and strip binaries to reduce AppImage size");
    
    m_resolveDepsCheckBox = new QCheckBox("Resolve dependencies", this);
    m_resolveDepsCheckBox->setStyleSheet(
        "QCheckBox {"
        "color: #e0e0e0;"
        "font-weight: 500;"
        "background: transparent;"
        "}"
        "QCheckBox::indicator {"
        "width: 18px;"
        "height: 18px;"
        "border: 2px solid rgba(108, 92, 231, 0.6);"
        "border-radius: 4px;"
        "background-color: rgba(0, 0, 0, 0.2);"
        "}"
        "QCheckBox::indicator:checked {"
        "background-color: rgba(108, 92, 231, 0.8);"
        "border-color: #6c5ce7;"
        "}"
        "QCheckBox::indicator:hover {"
        "border-color: #a29bfe;"
        "}"
    );
    m_resolveDepsCheckBox->setToolTip("Download missing library dependencies from repositories");
    
    QLabel* compressionLabel = new QLabel("Compression:", this);
    compressionLabel->setStyleSheet("color: #e0e0e0; font-weight: 500; background: transparent;");
    
    m_compressionComboBox = new QComboBox(this);
    m_compressionComboBox->addItem("Fast (gzip)", static_cast<int>(CompressionLevel::FAST));
    m_compressionComboBox->addItem("Normal (gzip)", static_cast<int>(CompressionLevel::NORMAL));
    m_compressionComboBox->addItem("Maximum (zstd)", static_cast<int>(CompressionLevel::MAXIMUM));
    m_compressionComboBox->addItem("Ultra (zstd)", static_cast<int>(CompressionLevel::ULTRA));
    m_compressionComboBox->setCurrentIndex(1);  // Default to Normal
    m_compressionComboBox->setStyleSheet(
        "QComboBox {"
        "background-color: rgba(255, 255, 255, 0.1);"
        "color: white;"
        "border: 1px solid rgba(255, 255, 255, 0.2);"
        "border-radius: 8px;"
        "padding: 6px 12px;"
        "min-width: 120px;"
        "}"
        "QComboBox:hover {"
        "border-color: rgba(108, 92, 231, 0.6);"
        "}"
        "QComboBox::drop-down {"
        "border: none;"
        "width: 20px;"
        "}"
        "QComboBox::down-arrow {"
        "image: none;"
        "border-left: 5px solid transparent;"
        "border-right: 5px solid transparent;"
        "border-top: 6px solid #e0e0e0;"
        "margin-right: 8px;"
        "}"
        "QComboBox QAbstractItemView {"
        "background-color: #1a1a2e;"
        "color: white;"
        "selection-background-color: rgba(108, 92, 231, 0.5);"
        "border: 1px solid rgba(255, 255, 255, 0.2);"
        "border-radius: 4px;"
        "}"
    );
    m_compressionComboBox->setToolTip("Choose compression level:\n"
        "Fast: Quick build, larger file\n"
        "Normal: Balanced (default)\n"
        "Maximum: Slower build, smaller file\n"
        "Ultra: Slowest build, smallest file");
    
    optimizeLayout->addWidget(m_optimizeCheckBox);
    optimizeLayout->addWidget(m_resolveDepsCheckBox);
    optimizeLayout->addStretch();
    optimizeLayout->addWidget(compressionLabel);
    optimizeLayout->addWidget(m_compressionComboBox);
    m_mainLayout->addLayout(optimizeLayout);
    
    // Build button with gradient
    m_buildButton = new QPushButton("Build AppImage", this);
    m_buildButton->setMinimumHeight(50);
    m_buildButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_buildButton->setStyleSheet(
        "QPushButton {"
        "background: qlineargradient(x1:0, y1:0, x2:1, y2:0, "
        "stop:0 #6c5ce7, stop:1 #4a90e2);"
        "color: white;"
        "font-weight: bold;"
        "font-size: 14px;"
        "border-radius: 10px;"
        "padding: 12px;"
        "border: none;"
        "}"
        "QPushButton:hover {"
        "background: qlineargradient(x1:0, y1:0, x2:1, y2:0, "
        "stop:0 #7d6ff0, stop:1 #5ba0f2);"
        "}"
        "QPushButton:pressed {"
        "background: qlineargradient(x1:0, y1:0, x2:1, y2:0, "
        "stop:0 #5a4cd4, stop:1 #3a7dd2);"
        "}"
        "QPushButton:disabled {"
        "background-color: rgba(100, 100, 100, 0.3);"
        "color: rgba(200, 200, 200, 0.5);"
        "}"
    );
    connect(m_buildButton, &QPushButton::clicked, this, &AppMainWindow::onBuildClicked);
    m_mainLayout->addWidget(m_buildButton);
    
    // Progress bar with modern styling
    m_progressBar = new QProgressBar(this);
    m_progressBar->setMinimum(0);
    m_progressBar->setMaximum(100);
    m_progressBar->setValue(0);
    m_progressBar->setTextVisible(true);
    m_progressBar->setFormat("%p% - %v");
    m_progressBar->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_progressBar->setMinimumHeight(30);
    m_progressBar->setStyleSheet(
        "QProgressBar {"
        "border: 2px solid rgba(255, 255, 255, 0.2);"
        "border-radius: 8px;"
        "text-align: center;"
        "color: #ffffff;"
        "font-weight: 500;"
        "background-color: rgba(0, 0, 0, 0.3);"
        "height: 30px;"
        "}"
        "QProgressBar::chunk {"
        "background: qlineargradient(x1:0, y1:0, x2:1, y2:0, "
        "stop:0 #6c5ce7, stop:1 #4a90e2);"
        "border-radius: 6px;"
        "}"
    );
    m_mainLayout->addWidget(m_progressBar);
    
    // Status label with dynamic colors
    m_statusLabel = new QLabel("Ready", this);
    m_statusLabel->setAlignment(Qt::AlignCenter);
    m_statusLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_statusLabel->setMinimumHeight(30);
    m_statusLabel->setStyleSheet(
        "QLabel {"
        "font-weight: bold;"
        "font-size: 13px;"
        "padding: 10px;"
        "color: #b0b0b0;"
        "background: transparent;"
        "}"
    );
    m_mainLayout->addWidget(m_statusLabel);
    
    // Log area with modern dark theme
    QLabel* logLabel = new QLabel("Log:", this);
    logLabel->setStyleSheet("color: #e0e0e0; font-weight: 500; background: transparent; padding-top: 5px;");
    logLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_logText = new QTextEdit(this);
    m_logText->setReadOnly(true);
    m_logText->setMinimumHeight(150);
    m_logText->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_logText->setFont(QFont("Monospace", 9));
    m_logText->setAcceptRichText(true);  // Enable HTML formatting for colored text
    m_logText->setStyleSheet(
        "QTextEdit {"
        "background-color: rgba(0, 0, 0, 0.5);"
        "color: #d4d4d4;"
        "border: 2px solid rgba(255, 255, 255, 0.1);"
        "border-radius: 10px;"
        "padding: 10px;"
        "selection-background-color: rgba(108, 92, 231, 0.3);"
        "}"
    );
    m_mainLayout->addWidget(logLabel);
    m_mainLayout->addWidget(m_logText, 1);  // Add stretch factor so log area expands
    
    // Add spacer before button to prevent overlap
    m_mainLayout->addSpacing(5);
    
    // Open output directory button with modern style
    m_openOutputDirButton = new QPushButton("Open Output Directory", this);
    m_openOutputDirButton->setEnabled(false);
    m_openOutputDirButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_openOutputDirButton->setMinimumHeight(40);
    m_openOutputDirButton->setStyleSheet(
        "QPushButton {"
        "background-color: rgba(74, 144, 226, 0.4);"
        "color: white;"
        "font-weight: 500;"
        "border-radius: 8px;"
        "padding: 10px;"
        "border: 1px solid rgba(74, 144, 226, 0.6);"
        "}"
        "QPushButton:hover {"
        "background-color: rgba(74, 144, 226, 0.6);"
        "border-color: rgba(74, 144, 226, 0.8);"
        "}"
        "QPushButton:pressed {"
        "background-color: rgba(74, 144, 226, 0.8);"
        "}"
        "QPushButton:disabled {"
        "background-color: rgba(100, 100, 100, 0.2);"
        "color: rgba(200, 200, 200, 0.4);"
        "border-color: rgba(100, 100, 100, 0.3);"
        "}"
    );
    connect(m_openOutputDirButton, &QPushButton::clicked, this, &AppMainWindow::onOpenOutputDirClicked);
    m_mainLayout->addWidget(m_openOutputDirButton);
}

void AppMainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls() && !m_isProcessing) {
        QList<QUrl> urls = event->mimeData()->urls();
        // Check if at least one file is valid
        for (const QUrl& url : urls) {
            QString filePath = url.toLocalFile();
            QFileInfo info(filePath);
            QString suffix = info.suffix().toLower();
            if (suffix == "deb" || suffix == "rpm" || TarballParser::isSupportedTarball(filePath)) {
                event->acceptProposedAction();
                animateDropAreaDragEnter();
                return;
            }
        }
    }
}

void AppMainWindow::dragLeaveEvent(QDragLeaveEvent* event) {
    Q_UNUSED(event);
    animateDropAreaDragLeave();
}

void AppMainWindow::dropEvent(QDropEvent* event) {
    animateDropAreaDrop();
    
    if (event->mimeData()->hasUrls() && !m_isProcessing) {
        QList<QUrl> urls = event->mimeData()->urls();
        QStringList validFiles;
        
        for (const QUrl& url : urls) {
            QString filePath = url.toLocalFile();
            QFileInfo info(filePath);
            QString suffix = info.suffix().toLower();
            if (suffix == "deb" || suffix == "rpm" || TarballParser::isSupportedTarball(filePath)) {
                validFiles.append(filePath);
            }
        }
        
        if (!validFiles.isEmpty()) {
            if (validFiles.size() == 1) {
                setPackageFile(validFiles.first());
            } else {
                setPackageFiles(validFiles);
            }
            event->acceptProposedAction();
        }
    }
}

void AppMainWindow::setPackageFile(const QString& filePath) {
    m_packageFilePath = filePath;
    m_packageFilePaths.clear();
    m_packageFilePaths.append(filePath);
    QFileInfo info(filePath);
    m_fileLabel->setText(QString("Selected: %1").arg(info.fileName()));
    m_fileLabel->setStyleSheet(
        "QLabel {"
        "color: #6c5ce7;"
        "font-weight: bold;"
        "font-size: 12px;"
        "padding: 8px;"
        "background: transparent;"
        "}"
    );
    m_buildButton->setEnabled(true);
}

void AppMainWindow::setPackageFiles(const QStringList& filePaths) {
    m_packageFilePaths = filePaths;
    m_packageFilePath = filePaths.first();
    
    // Build list of filenames for display
    QStringList fileNames;
    for (const QString& path : filePaths) {
        fileNames.append(QFileInfo(path).fileName());
    }
    
    QString displayText;
    if (filePaths.size() <= 3) {
        displayText = QString("Selected %1 files: %2").arg(filePaths.size()).arg(fileNames.join(", "));
    } else {
        displayText = QString("Selected %1 files: %2, ...").arg(filePaths.size()).arg(fileNames.mid(0, 2).join(", "));
    }
    
    m_fileLabel->setText(displayText);
    m_fileLabel->setStyleSheet(
        "QLabel {"
        "color: #6c5ce7;"
        "font-weight: bold;"
        "font-size: 12px;"
        "padding: 8px;"
        "background: transparent;"
        "}"
    );
    m_buildButton->setEnabled(true);
    m_buildButton->setText(QString("Build %1 AppImages").arg(filePaths.size()));
}

void AppMainWindow::onSelectOutputDirClicked() {
    QString dir = QFileDialog::getExistingDirectory(this, "Select Output Directory", m_outputDir);
    if (!dir.isEmpty()) {
        m_outputDir = dir;
        m_outputDirLabel->setText(dir);
        animateOutputDirSelection();
    }
}

void AppMainWindow::onSearchClicked() {
    AppStoreWindow dialog(this);
    connect(&dialog, &AppStoreWindow::packageSelected, this, &AppMainWindow::onPackageSelected);
    dialog.exec();
}

void AppMainWindow::onPackageSelected(const QString& packagePath) {
    if (!packagePath.isEmpty()) {
        setPackageFile(packagePath);
    }
}

void AppMainWindow::onBuildClicked() {
    if (m_packageFilePaths.isEmpty()) {
        QMessageBox::warning(this, "No file selected", "Please select a package file first (.deb, .rpm, or archive).");
        return;
    }
    
    if (m_isProcessing) {
        m_conversionController->cancel();
        return;
    }
    
    resetUI();
    m_isProcessing = true;
    m_conversionStartTime = QDateTime::currentMSecsSinceEpoch();
    enableControls(false);
    m_buildButton->setText("Cancel");
    
    if (m_packageFilePaths.size() > 1) {
        m_statusLabel->setText(QString("Processing batch: 1/%1...").arg(m_packageFilePaths.size()));
    } else {
        m_statusLabel->setText("Processing...");
    }
    m_statusLabel->setStyleSheet(
        "QLabel {"
        "font-weight: bold;"
        "font-size: 13px;"
        "color: #4a90e2;"
        "padding: 10px;"
        "background: transparent;"
        "}"
    );

    ConversionRequest request;
    request.packagePaths = m_packageFilePaths;
    request.outputDir = m_outputDir;

    OptimizationSettings optSettings;
    optSettings.enabled = m_optimizeCheckBox->isChecked();
    optSettings.compression = static_cast<CompressionLevel>(
        m_compressionComboBox->currentData().toInt());
    request.optimizationSettings = optSettings;

    DependencySettings depSettings;
    depSettings.enabled = m_resolveDepsCheckBox->isChecked();
    request.dependencySettings = depSettings;

    m_conversionController->start(request);
}

void AppMainWindow::onPackageStarted(int index, int totalCount, const QString& packagePath) {
    const QFileInfo packageInfo(packagePath);

    if (totalCount > 1) {
        onLog(QString("=== [%1/%2] Converting: %3 ===")
            .arg(index + 1)
            .arg(totalCount)
            .arg(packageInfo.fileName()));
        m_statusLabel->setText(QString("Processing %1/%2: %3...")
            .arg(index + 1)
            .arg(totalCount)
            .arg(packageInfo.fileName()));
    }
}

void AppMainWindow::onProgress(int percentage, const QString& message) {
    qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - m_conversionStartTime;
    // Hide progress bar if conversion is fast (< 1.5 seconds)
    if (elapsed < 1500) {
        m_progressBar->hide();
    } else {
        m_progressBar->show();
        animateProgressBar(percentage);
    }
    m_statusLabel->setText(message);
}

void AppMainWindow::onLog(const QString& message) {
    QString coloredMessage = formatLogMessage(message);
    m_logText->append(coloredMessage);
    QTextCursor cursor = m_logText->textCursor();
    cursor.movePosition(QTextCursor::End);
    m_logText->setTextCursor(cursor);
}

QString AppMainWindow::formatLogMessage(const QString& message) {
    // Escape HTML special characters
    QString escaped = message.toHtmlEscaped();
    
    // Color scheme inspired by zsh (Oh My Zsh theme)
    // ERROR - bright red (bold)
    QRegularExpression errorRegex("(ERROR:.*)", QRegularExpression::CaseInsensitiveOption);
    escaped.replace(errorRegex, R"(<span style="color: #ff6b6b; font-weight: bold;">\1</span>)");
    
    // WARNING - orange/yellow (bold)
    QRegularExpression warningRegex("(WARNING:.*)", QRegularExpression::CaseInsensitiveOption);
    escaped.replace(warningRegex, R"(<span style="color: #ffa502; font-weight: bold;">\1</span>)");
    
    // SUCCESS messages - green (bold)
    QRegularExpression successRegex("(Successfully.*|completed successfully.*|✓.*)", QRegularExpression::CaseInsensitiveOption);
    escaped.replace(successRegex, R"(<span style="color: #51cf66; font-weight: bold;">\1</span>)");
    
    // File paths with extensions - purple
    QRegularExpression fileRegex(R"((/[^\s<>"]+\.(deb|rpm|AppImage|desktop|py|sh)))");
    escaped.replace(fileRegex, R"(<span style="color: #a78bfa;">\1</span>)");
    
    // Package names and versions - cyan
    QRegularExpression packageRegex("(Package:|Version:)\\s*([\\w.-]+)", QRegularExpression::CaseInsensitiveOption);
    escaped.replace(packageRegex, R"(<span style="color: #4dabf7; font-weight: bold;">\1</span> <span style="color: #66d9ef;">\2</span>)");
    
    // INFO/Status messages - blue
    QRegularExpression infoRegex("(=== .* ===|Building.*|Copying.*|Found.*|Running.*|AppImageTool.*)", QRegularExpression::CaseInsensitiveOption);
    escaped.replace(infoRegex, R"(<span style="color: #4dabf7;">\1</span>)");
    
    // Tips and hints - dimmed cyan (italic)
    QRegularExpression tipRegex("(TIP:|Tip:.*)", QRegularExpression::CaseInsensitiveOption);
    escaped.replace(tipRegex, R"(<span style="color: #74c0fc; font-style: italic;">\1</span>)");
    
    // Directory paths - dimmed white
    QRegularExpression dirRegex(R"((/[^\s<>"]+/))");
    escaped.replace(dirRegex, R"(<span style="color: #adb5bd;">\1</span>)");
    
    // Numbers - yellow (but not inside HTML tags)
    QRegularExpression numberRegex(R"(\b(\d+)\b)");
    escaped.replace(numberRegex, R"(<span style="color: #ffd43b;">\1</span>)");
    
    return escaped;
}

void AppMainWindow::onError(const QString& errorMessage) {
    if (m_packageFilePaths.size() > 1) {
        m_statusLabel->setText(errorMessage);
    } else {
        m_statusLabel->setText("Error: " + errorMessage);
        QMessageBox::critical(this, "Error", errorMessage);
    }
    
    m_statusLabel->setStyleSheet(
        "QLabel {"
        "font-weight: bold;"
        "font-size: 13px;"
        "color: #e74c3c;"
        "padding: 10px;"
        "background: transparent;"
        "}"
    );
}

void AppMainWindow::onSuccess(const QString& appImagePath) {
    if (m_packageFilePaths.size() > 1) {
        m_statusLabel->setText(QString("Created %1")
            .arg(QFileInfo(appImagePath).fileName()));
    } else {
        m_statusLabel->setText("Success! AppImage created.");
        m_openOutputDirButton->setEnabled(true);
        
        // Animate success
        animateSuccess();
        
        QMessageBox::information(this, "Success", 
            QString("AppImage created successfully!\n\n%1").arg(appImagePath));
    }
    
    m_statusLabel->setStyleSheet(
        "QLabel {"
        "font-weight: bold;"
        "font-size: 13px;"
        "color: #2ecc71;"
        "padding: 10px;"
        "background: transparent;"
        "}"
    );
}

void AppMainWindow::onConversionFinished(int successCount, int failureCount, bool cancelled) {
    m_isProcessing = false;
    enableControls(true);

    if (m_packageFilePaths.size() > 1) {
        m_buildButton->setText(QString("Build %1 AppImages").arg(m_packageFilePaths.size()));
    } else {
        m_buildButton->setText("Build AppImage");
    }
    m_buildButton->setEnabled(!m_packageFilePaths.isEmpty());

    if (cancelled) {
        m_statusLabel->setText("Conversion cancelled.");
        m_statusLabel->setStyleSheet(
            "QLabel { font-weight: bold; font-size: 13px; color: #f39c12; padding: 10px; background: transparent; }"
        );
        m_openOutputDirButton->setEnabled(successCount > 0);
        return;
    }

    if (m_packageFilePaths.size() > 1) {
        QString resultMsg;
        QString resultStyle;

        if (failureCount == 0) {
            resultMsg = QString("Batch complete: All %1 packages converted successfully!").arg(successCount);
            resultStyle = "QLabel { font-weight: bold; font-size: 13px; color: #2ecc71; padding: 10px; background: transparent; }";
        } else if (successCount == 0) {
            resultMsg = QString("Batch failed: All %1 packages failed to convert").arg(failureCount);
            resultStyle = "QLabel { font-weight: bold; font-size: 13px; color: #e74c3c; padding: 10px; background: transparent; }";
        } else {
            resultMsg = QString("Batch complete: %1 succeeded, %2 failed").arg(successCount).arg(failureCount);
            resultStyle = "QLabel { font-weight: bold; font-size: 13px; color: #f39c12; padding: 10px; background: transparent; }";
        }

        m_statusLabel->setText(resultMsg);
        m_statusLabel->setStyleSheet(resultStyle);
        QMessageBox::information(this, "Batch Conversion Complete", resultMsg);
    }

    m_openOutputDirButton->setEnabled(successCount > 0);
}

void AppMainWindow::onSudoPasswordRequested(const QString& packagePath, const QString& reason) {
    bool ok = false;
    const QString password = QInputDialog::getText(
        this,
        tr("Sudo Password Required"),
        tr("%1\n\nPackage: %2\n\nEnter your password or leave it empty to continue without it.")
            .arg(reason, QFileInfo(packagePath).fileName()),
        QLineEdit::Password,
        QString(),
        &ok
    );

    if (!ok) {
        onLog("WARNING: Sudo password dialog was dismissed. Continuing without dependency download credentials.");
        m_conversionController->continueWithoutSudoPassword();
        return;
    }

    if (password.isEmpty()) {
        onLog("WARNING: No sudo password provided. Dependency resolution may fail for pacman packages.");
        m_conversionController->continueWithoutSudoPassword();
        return;
    }

    m_conversionController->provideSudoPassword(password);
}

void AppMainWindow::onOpenOutputDirClicked() {
    QString dir = m_outputDir.isEmpty() ? QDir::homePath() : m_outputDir;
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

bool AppMainWindow::eventFilter(QObject* obj, QEvent* event) {
    if (obj == m_dropArea && event->type() == QEvent::MouseButtonPress && !m_isProcessing) {
        QMouseEvent* mouseEvent = static_cast<QMouseEvent*>(event);
        if (mouseEvent->button() == Qt::LeftButton) {
            QString filePath = QFileDialog::getOpenFileName(this, 
                "Select package file", 
                QDir::homePath(),
                "All supported (*.deb *.rpm *.tar.gz *.tgz *.tar.xz *.txz *.tar.bz2 *.tar.zst *.zip *.tar);;"
                "Debian packages (*.deb);;RPM packages (*.rpm);;" + TarballParser::getFileFilter());
            if (!filePath.isEmpty()) {
                setPackageFile(filePath);
            }
            return true;
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

void AppMainWindow::resetUI() {
    m_currentProgress = 0;
    m_progressBar->setValue(0);
    m_statusLabel->setText("Ready");
    m_statusLabel->setStyleSheet(
        "QLabel {"
        "font-weight: bold;"
        "font-size: 13px;"
        "padding: 10px;"
        "color: #b0b0b0;"
        "background: transparent;"
        "}"
    );
    m_logText->clear();
    m_openOutputDirButton->setEnabled(false);
    m_progressBar->show();
    
    // Reset graphics effects
    m_statusLabel->setGraphicsEffect(nullptr);
    m_openOutputDirButton->setGraphicsEffect(nullptr);
}

void AppMainWindow::enableControls(bool enabled) {
    m_selectOutputDirButton->setEnabled(enabled);
    m_optimizeCheckBox->setEnabled(enabled);
    m_resolveDepsCheckBox->setEnabled(enabled);
    m_compressionComboBox->setEnabled(enabled);
    m_searchButton->setEnabled(enabled);
    // Don't disable build button - it becomes cancel button
}

// Animation implementations
void AppMainWindow::animateDropAreaDragEnter() {
    // Stop any existing animation
    if (m_dropAreaAnimation && m_dropAreaAnimation->state() == QAbstractAnimation::Running) {
        m_dropAreaAnimation->stop();
    }
    
    // Animate shadow effect
    m_dropAreaAnimation = new QPropertyAnimation(m_dropAreaShadow, "blurRadius", this);
    m_dropAreaAnimation->setDuration(300);
    m_dropAreaAnimation->setEasingCurve(QEasingCurve::OutCubic);
    m_dropAreaAnimation->setStartValue(15);
    m_dropAreaAnimation->setEndValue(30);
    m_dropAreaAnimation->start(QAbstractAnimation::DeleteWhenStopped);
    
    // Animate color
    QPropertyAnimation* colorAnim = new QPropertyAnimation(m_dropAreaShadow, "color", this);
    colorAnim->setDuration(300);
    colorAnim->setEasingCurve(QEasingCurve::OutCubic);
    colorAnim->setStartValue(QColor(108, 92, 231, 100));
    colorAnim->setEndValue(QColor(162, 155, 254, 200));
    colorAnim->start(QAbstractAnimation::DeleteWhenStopped);
    
    // Update style with animation
    m_dropArea->setStyleSheet(
        "QLabel {"
        "border: 3px dashed #a29bfe;"
        "border-radius: 15px;"
        "background: qlineargradient(x1:0, y1:0, x2:1, y2:1, "
        "stop:0 rgba(108, 92, 231, 0.3), stop:1 rgba(74, 144, 226, 0.3));"
        "padding: 30px;"
        "color: #ffffff;"
        "font-size: 14px;"
        "}"
    );
}

void AppMainWindow::animateDropAreaDragLeave() {
    animateDropAreaDrop();
}

void AppMainWindow::animateDropAreaDrop() {
    // Stop any existing animation
    if (m_dropAreaAnimation && m_dropAreaAnimation->state() == QAbstractAnimation::Running) {
        m_dropAreaAnimation->stop();
    }
    
    // Animate shadow back
    m_dropAreaAnimation = new QPropertyAnimation(m_dropAreaShadow, "blurRadius", this);
    m_dropAreaAnimation->setDuration(300);
    m_dropAreaAnimation->setEasingCurve(QEasingCurve::OutCubic);
    m_dropAreaAnimation->setStartValue(m_dropAreaShadow->blurRadius());
    m_dropAreaAnimation->setEndValue(15);
    m_dropAreaAnimation->start(QAbstractAnimation::DeleteWhenStopped);
    
    // Animate color back
    QPropertyAnimation* colorAnim = new QPropertyAnimation(m_dropAreaShadow, "color", this);
    colorAnim->setDuration(300);
    colorAnim->setEasingCurve(QEasingCurve::OutCubic);
    colorAnim->setStartValue(m_dropAreaShadow->color());
    colorAnim->setEndValue(QColor(108, 92, 231, 100));
    colorAnim->start(QAbstractAnimation::DeleteWhenStopped);
    
    // Reset style
    m_dropArea->setStyleSheet(
        "QLabel {"
        "border: 3px dashed #6c5ce7;"
        "border-radius: 15px;"
        "background: qlineargradient(x1:0, y1:0, x2:1, y2:1, "
        "stop:0 rgba(108, 92, 231, 0.1), stop:1 rgba(74, 144, 226, 0.1));"
        "padding: 30px;"
        "color: #d0d0d0;"
        "font-size: 14px;"
        "}"
        "QLabel:hover {"
        "border-color: #a29bfe;"
        "background: qlineargradient(x1:0, y1:0, x2:1, y2:1, "
        "stop:0 rgba(108, 92, 231, 0.2), stop:1 rgba(74, 144, 226, 0.2));"
        "}"
    );
}

void AppMainWindow::animateOutputDirSelection() {
    // Create a fade and scale animation for the output directory label
    QGraphicsOpacityEffect* opacityEffect = new QGraphicsOpacityEffect(m_outputDirLabel);
    m_outputDirLabel->setGraphicsEffect(opacityEffect);
    
    QPropertyAnimation* fadeAnim = new QPropertyAnimation(opacityEffect, "opacity", this);
    fadeAnim->setDuration(400);
    fadeAnim->setEasingCurve(QEasingCurve::InOutQuad);
    fadeAnim->setStartValue(0.3);
    fadeAnim->setEndValue(1.0);
    fadeAnim->start(QAbstractAnimation::DeleteWhenStopped);
    
    // Pulse effect
    QTimer::singleShot(100, [this]() {
        animateButtonPulse(m_selectOutputDirButton);
    });
}

void AppMainWindow::animateProgressBar(int targetValue) {
    if (m_progressAnimation->state() == QAbstractAnimation::Running) {
        m_progressAnimation->stop();
    }
    
    m_progressAnimation->setStartValue(m_currentProgress);
    m_progressAnimation->setEndValue(targetValue);
    m_currentProgress = targetValue;
    
    // Adjust duration based on progress change
    int change = abs(targetValue - m_progressAnimation->startValue().toInt());
    m_progressAnimation->setDuration(qMin(500, change * 5));
    
    m_progressAnimation->start();
}

void AppMainWindow::animateSuccess() {
    // Animate progress bar to 100% with a bounce effect
    if (m_progressAnimation->state() == QAbstractAnimation::Running) {
        m_progressAnimation->stop();
    }
    
    m_progressAnimation->setStartValue(m_currentProgress);
    m_progressAnimation->setEndValue(100);
    m_progressAnimation->setDuration(600);
    m_progressAnimation->setEasingCurve(QEasingCurve::OutBounce);
    m_progressAnimation->start();
    
    // Animate status label with pulse
    QGraphicsOpacityEffect* statusEffect = new QGraphicsOpacityEffect(m_statusLabel);
    m_statusLabel->setGraphicsEffect(statusEffect);
    
    QPropertyAnimation* pulseAnim = new QPropertyAnimation(statusEffect, "opacity", this);
    pulseAnim->setDuration(1500);
    pulseAnim->setEasingCurve(QEasingCurve::InOutSine);
    pulseAnim->setKeyValueAt(0, 1.0);
    pulseAnim->setKeyValueAt(0.3, 0.5);
    pulseAnim->setKeyValueAt(0.6, 1.0);
    pulseAnim->setKeyValueAt(0.9, 0.7);
    pulseAnim->setKeyValueAt(1.0, 1.0);
    pulseAnim->start(QAbstractAnimation::DeleteWhenStopped);
    
    // Animate success button appearance
    m_openOutputDirButton->setGraphicsEffect(nullptr);
    QGraphicsOpacityEffect* buttonEffect = new QGraphicsOpacityEffect(m_openOutputDirButton);
    m_openOutputDirButton->setGraphicsEffect(buttonEffect);
    buttonEffect->setOpacity(0.0);
    
    QPropertyAnimation* buttonFade = new QPropertyAnimation(buttonEffect, "opacity", this);
    buttonFade->setDuration(500);
    buttonFade->setEasingCurve(QEasingCurve::OutCubic);
    buttonFade->setStartValue(0.0);
    buttonFade->setEndValue(1.0);
    buttonFade->start(QAbstractAnimation::DeleteWhenStopped);
    
    // Start pulsing animation for the button
    QTimer::singleShot(500, [this]() {
        animateButtonPulse(m_openOutputDirButton);
    });
}

void AppMainWindow::animateButtonPulse(QPushButton* button) {
    if (!button) return;
    
    QGraphicsDropShadowEffect* shadow = qobject_cast<QGraphicsDropShadowEffect*>(button->graphicsEffect());
    if (!shadow) {
        shadow = new QGraphicsDropShadowEffect(button);
        shadow->setBlurRadius(10);
        shadow->setColor(QColor(74, 144, 226, 150));
        shadow->setOffset(0, 2);
        button->setGraphicsEffect(shadow);
    }
    
    if (m_buttonPulseAnimation && m_buttonPulseAnimation->state() == QAbstractAnimation::Running) {
        m_buttonPulseAnimation->stop();
    }
    
    m_buttonPulseAnimation = new QPropertyAnimation(shadow, "blurRadius", this);
    m_buttonPulseAnimation->setDuration(1000);
    m_buttonPulseAnimation->setEasingCurve(QEasingCurve::InOutSine);
    m_buttonPulseAnimation->setStartValue(10);
    m_buttonPulseAnimation->setEndValue(20);
    m_buttonPulseAnimation->setLoopCount(3);
    m_buttonPulseAnimation->start(QAbstractAnimation::DeleteWhenStopped);
    
    // Also animate color
    QPropertyAnimation* colorAnim = new QPropertyAnimation(shadow, "color", this);
    colorAnim->setDuration(1000);
    colorAnim->setEasingCurve(QEasingCurve::InOutSine);
    colorAnim->setStartValue(QColor(74, 144, 226, 150));
    colorAnim->setEndValue(QColor(108, 92, 231, 200));
    colorAnim->setLoopCount(3);
    colorAnim->start(QAbstractAnimation::DeleteWhenStopped);
}
