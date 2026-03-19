#include "app_details_dialog.h"
#include <QDesktopServices>
#include <QUrl>
#include <QFileInfo>
#include <QTimer>
#include <QPainter>
#include "appstream_metadata.h"

AppDetailsDialog::AppDetailsDialog(const AppInfo& appInfo, QWidget* parent)
    : QDialog(parent)
    , m_appInfo(appInfo)
{
    QString displayName = !appInfo.displayName.isEmpty() ? appInfo.displayName : appInfo.name;
    setWindowTitle(QString("Details: %1").arg(displayName));
    setMinimumSize(700, 600);
    resize(800, 700);
    
    setupUI();
    loadScreenshots();
}

AppDetailsDialog::~AppDetailsDialog() {
}

void AppDetailsDialog::setupUI() {
    setStyleSheet(
        "QDialog {"
        "background: qlineargradient(x1:0, y1:0, x2:0, y2:1, "
        "stop:0 #1a1a2e, stop:0.5 #16213e, stop:1 #0f3460);"
        "color: #e0e0e0;"
        "}"
    );
    
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(20);
    mainLayout->setContentsMargins(30, 30, 30, 30);
    
    QScrollArea* scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setStyleSheet(
        "QScrollArea {"
        "background: transparent;"
        "border: none;"
        "}"
        "QScrollBar:vertical {"
        "background: rgba(0, 0, 0, 0.3);"
        "width: 12px;"
        "border-radius: 6px;"
        "}"
        "QScrollBar::handle:vertical {"
        "background: rgba(108, 92, 231, 0.6);"
        "border-radius: 6px;"
        "min-height: 30px;"
        "}"
        "QScrollBar::handle:vertical:hover {"
        "background: rgba(108, 92, 231, 0.8);"
        "}"
    );
    
    QWidget* scrollContent = new QWidget();
    QVBoxLayout* contentLayout = new QVBoxLayout(scrollContent);
    contentLayout->setSpacing(20);
    contentLayout->setContentsMargins(20, 20, 20, 20);
    
    // Header section with icon and basic info
    QHBoxLayout* headerLayout = new QHBoxLayout();
    headerLayout->setSpacing(20);
    
    // Icon
    m_iconLabel = new QLabel(this);
    m_iconLabel->setFixedSize(128, 128);
    m_iconLabel->setAlignment(Qt::AlignCenter);
    m_iconLabel->setScaledContents(false);
    
    if (!m_appInfo.icon.isNull()) {
        QPixmap icon = m_appInfo.icon.scaled(128, 128, 
                                             Qt::KeepAspectRatio, 
                                             Qt::SmoothTransformation);
        m_iconLabel->setPixmap(icon);
    } else {
        // Default icon placeholder
        QPixmap placeholder(128, 128);
        placeholder.fill(QColor(60, 60, 80));
        QPainter painter(&placeholder);
        painter.setPen(QColor(150, 150, 150));
        painter.setFont(QFont("Arial", 48, QFont::Bold));
        QString initial = m_appInfo.name.isEmpty() ? "?" : m_appInfo.name.left(1).toUpper();
        painter.drawText(placeholder.rect(), Qt::AlignCenter, initial);
        m_iconLabel->setPixmap(placeholder);
        
        // Load icon asynchronously
        QTimer::singleShot(0, this, [this]() {
            AppStreamMetadata* metadata = new AppStreamMetadata(this);
            QString iconLookupName = !m_appInfo.packageName.isEmpty() ? m_appInfo.packageName : m_appInfo.name;
            QPixmap icon = metadata->findIconForPackage(iconLookupName, 128);
            if (icon.isNull()) {
                icon = metadata->getIcon(iconLookupName, 128);
            }
            if (!icon.isNull()) {
                m_iconLabel->setPixmap(icon.scaled(128, 128, 
                                                   Qt::KeepAspectRatio, 
                                                   Qt::SmoothTransformation));
            }
            metadata->deleteLater();
        });
    }
    
    headerLayout->addWidget(m_iconLabel);
    
    // Name and version
    QVBoxLayout* nameLayout = new QVBoxLayout();
    nameLayout->setSpacing(5);
    
    m_nameLabel = new QLabel(this);
    QString displayName = !m_appInfo.displayName.isEmpty() ? m_appInfo.displayName : m_appInfo.name;
    // Use proper app name, not package name
    m_nameLabel->setText(displayName);
    m_nameLabel->setWordWrap(true);
    m_nameLabel->setTextFormat(Qt::PlainText);
    m_nameLabel->setStyleSheet(
        "QLabel {"
        "color: #ffffff;"
        "font-size: 28px;"
        "font-weight: bold;"
        "background: transparent;"
        "line-height: 1.2;"
        "}"
    );
    nameLayout->addWidget(m_nameLabel);
    
    m_versionLabel = new QLabel(this);
    m_versionLabel->setText(QString("Version: %1").arg(m_appInfo.version));
    m_versionLabel->setStyleSheet(
        "QLabel {"
        "color: #a0a0a0;"
        "font-size: 14px;"
        "background: transparent;"
        "}"
    );
    nameLayout->addWidget(m_versionLabel);
    
    if (!m_appInfo.developerName.isEmpty()) {
        m_developerLabel = new QLabel(this);
        m_developerLabel->setText(QString("Developer: %1").arg(m_appInfo.developerName));
        m_developerLabel->setStyleSheet(
            "QLabel {"
            "color: #888888;"
            "font-size: 12px;"
            "background: transparent;"
            "}"
        );
        nameLayout->addWidget(m_developerLabel);
    } else {
        m_developerLabel = nullptr;
    }
    
    nameLayout->addStretch();
    headerLayout->addLayout(nameLayout, 1);
    
    contentLayout->addLayout(headerLayout);
    
    // Description
    if (!m_appInfo.description.isEmpty()) {
        m_descriptionText = new QTextEdit(this);
        m_descriptionText->setPlainText(m_appInfo.description);
        m_descriptionText->setReadOnly(true);
        m_descriptionText->setMaximumHeight(80);
        m_descriptionText->setStyleSheet(
            "QTextEdit {"
            "background: rgba(0, 0, 0, 0.3);"
            "color: #e0e0e0;"
            "border: 1px solid rgba(108, 92, 231, 0.3);"
            "border-radius: 8px;"
            "padding: 10px;"
            "font-size: 14px;"
            "}"
        );
        contentLayout->addWidget(m_descriptionText);
    } else {
        m_descriptionText = nullptr;
    }
    
    // Long description
    if (!m_appInfo.longDescription.isEmpty()) {
        QLabel* longDescLabel = new QLabel("Description:", this);
        longDescLabel->setStyleSheet(
            "QLabel {"
            "color: #ffffff;"
            "font-size: 16px;"
            "font-weight: bold;"
            "background: transparent;"
            "}"
        );
        contentLayout->addWidget(longDescLabel);
        
        m_longDescriptionText = new QTextEdit(this);
        m_longDescriptionText->setPlainText(m_appInfo.longDescription);
        m_longDescriptionText->setReadOnly(true);
        m_longDescriptionText->setMinimumHeight(150);
        m_longDescriptionText->setStyleSheet(
            "QTextEdit {"
            "background: rgba(0, 0, 0, 0.3);"
            "color: #e0e0e0;"
            "border: 1px solid rgba(108, 92, 231, 0.3);"
            "border-radius: 8px;"
            "padding: 10px;"
            "font-size: 13px;"
            "}"
        );
        contentLayout->addWidget(m_longDescriptionText);
    } else {
        m_longDescriptionText = nullptr;
    }
    
    // Screenshots
    if (!m_appInfo.screenshots.isEmpty() || !m_appInfo.screenshotPaths.isEmpty()) {
        QLabel* screenshotsLabel = new QLabel("Screenshots:", this);
        screenshotsLabel->setStyleSheet(
            "QLabel {"
            "color: #ffffff;"
            "font-size: 16px;"
            "font-weight: bold;"
            "background: transparent;"
            "}"
        );
        contentLayout->addWidget(screenshotsLabel);
        
        m_screenshotsList = new QListWidget(this);
        m_screenshotsList->setViewMode(QListWidget::IconMode);
        m_screenshotsList->setIconSize(QSize(400, 300));
        m_screenshotsList->setResizeMode(QListWidget::Adjust);
        m_screenshotsList->setSpacing(10);
        m_screenshotsList->setStyleSheet(
            "QListWidget {"
            "background: rgba(0, 0, 0, 0.3);"
            "border: 1px solid rgba(108, 92, 231, 0.3);"
            "border-radius: 8px;"
            "padding: 10px;"
            "}"
            "QListWidget::item {"
            "background: rgba(40, 40, 60, 0.5);"
            "border-radius: 8px;"
            "margin: 5px;"
            "}"
            "QListWidget::item:hover {"
            "background: rgba(60, 60, 80, 0.7);"
            "}"
        );
        m_screenshotsList->setMaximumHeight(350);
        contentLayout->addWidget(m_screenshotsList);
    } else {
        m_screenshotsList = nullptr;
    }
    
    // Information grid
    QGridLayout* infoLayout = new QGridLayout();
    infoLayout->setSpacing(15);
    
    int row = 0;
    
    m_sizeLabel = new QLabel(this);
    m_sizeLabel->setText(QString("Size: %1").arg(m_appInfo.sizeFormatted()));
    m_sizeLabel->setStyleSheet(
        "QLabel {"
        "color: #c0c0c0;"
        "font-size: 13px;"
        "background: transparent;"
        "}"
    );
    infoLayout->addWidget(m_sizeLabel, row, 0);
    
    if (!m_appInfo.license.isEmpty()) {
        m_licenseLabel = new QLabel(this);
        m_licenseLabel->setText(QString("License: %1").arg(m_appInfo.license));
        m_licenseLabel->setStyleSheet(
            "QLabel {"
            "color: #c0c0c0;"
            "font-size: 13px;"
            "background: transparent;"
            "}"
        );
        infoLayout->addWidget(m_licenseLabel, row, 1);
        row++;
    } else {
        m_licenseLabel = nullptr;
    }
    
    if (!m_appInfo.categories.isEmpty()) {
        QString categoriesStr = m_appInfo.categories.join(", ");
        m_categoryLabel = new QLabel(this);
        m_categoryLabel->setText(QString("Categories: %1").arg(categoriesStr));
        m_categoryLabel->setWordWrap(true);
        m_categoryLabel->setStyleSheet(
            "QLabel {"
            "color: #c0c0c0;"
            "font-size: 13px;"
            "background: transparent;"
            "}"
        );
        infoLayout->addWidget(m_categoryLabel, row, 0, 1, 2);
        row++;
    } else {
        m_categoryLabel = nullptr;
    }
    
    if (!m_appInfo.repository.isEmpty()) {
        m_repositoryLabel = new QLabel(this);
        m_repositoryLabel->setText(QString("Repository: %1").arg(m_appInfo.repository));
        m_repositoryLabel->setStyleSheet(
            "QLabel {"
            "color: #c0c0c0;"
            "font-size: 13px;"
            "background: transparent;"
            "}"
        );
        infoLayout->addWidget(m_repositoryLabel, row, 0, 1, 2);
    } else {
        m_repositoryLabel = nullptr;
    }
    
    contentLayout->addLayout(infoLayout);
    contentLayout->addStretch();
    
    scrollArea->setWidget(scrollContent);
    mainLayout->addWidget(scrollArea, 1);
    
    // Buttons
    QHBoxLayout* buttonLayout = new QHBoxLayout();
    buttonLayout->setSpacing(15);
    
    m_downloadButton = new QPushButton("Download & Convert", this);
    m_downloadButton->setStyleSheet(
        "QPushButton {"
        "background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #27ae60, stop:1 #2ecc71);"
        "color: white;"
        "font-weight: bold;"
        "border-radius: 8px;"
        "padding: 12px 30px;"
        "border: none;"
        "}"
        "QPushButton:hover {"
        "background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #2ecc71, stop:1 #3dd884);"
        "}"
    );
    connect(m_downloadButton, &QPushButton::clicked, this, &AppDetailsDialog::onDownloadButtonClicked);
    buttonLayout->addWidget(m_downloadButton);
    
    if (!m_appInfo.homepage.isEmpty()) {
        m_viewInRepositoryButton = new QPushButton("View Homepage", this);
    } else {
        m_viewInRepositoryButton = new QPushButton("View in Repository", this);
    }
    m_viewInRepositoryButton->setStyleSheet(
        "QPushButton {"
        "background: rgba(108, 92, 231, 0.6);"
        "color: white;"
        "font-weight: 500;"
        "border-radius: 8px;"
        "padding: 12px 30px;"
        "border: 1px solid rgba(108, 92, 231, 0.8);"
        "}"
        "QPushButton:hover {"
        "background: rgba(108, 92, 231, 0.8);"
        "}"
    );
    connect(m_viewInRepositoryButton, &QPushButton::clicked, this, &AppDetailsDialog::onViewInRepositoryClicked);
    buttonLayout->addWidget(m_viewInRepositoryButton);
    
    buttonLayout->addStretch();
    mainLayout->addLayout(buttonLayout);
}

void AppDetailsDialog::loadScreenshots() {
    if (!m_screenshotsList) {
        return;
    }
    
    // Load screenshots from paths
    for (const QString& path : m_appInfo.screenshotPaths) {
        if (QFileInfo::exists(path)) {
            QPixmap pixmap(path);
            if (!pixmap.isNull()) {
                QListWidgetItem* item = new QListWidgetItem();
                item->setIcon(QIcon(pixmap.scaled(400, 300, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
                m_screenshotsList->addItem(item);
            }
        }
    }
    
    // Also add from loaded screenshots
    for (const QPixmap& screenshot : m_appInfo.screenshots) {
        if (!screenshot.isNull()) {
            QListWidgetItem* item = new QListWidgetItem();
            item->setIcon(QIcon(screenshot.scaled(400, 300, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
            m_screenshotsList->addItem(item);
        }
    }
}

void AppDetailsDialog::onDownloadButtonClicked() {
    emit downloadClicked(m_appInfo);
    accept();
}

void AppDetailsDialog::onViewInRepositoryClicked() {
    if (!m_appInfo.homepage.isEmpty()) {
        QDesktopServices::openUrl(QUrl(m_appInfo.homepage));
    } else if (!m_appInfo.downloadUrl.isEmpty()) {
        QDesktopServices::openUrl(QUrl(m_appInfo.downloadUrl));
    }
}

