#include "app_card_widget.h"
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QEvent>
#include <QEnterEvent>
#include <QTimer>
#include <QDebug>
#include <cstdio>
#include "appstream_metadata.h"

AppCardWidget::AppCardWidget(const AppInfo& appInfo, QWidget* parent)
    : QWidget(parent)
    , m_appInfo(appInfo)
    , m_opacity(1.0)
    , m_hovered(false)
{
    setFixedSize(CARD_WIDTH, CARD_HEIGHT);
    setCursor(Qt::PointingHandCursor);
    
    setupUI();
    updateCardStyle();
    
    // Setup hover animation
    m_hoverAnimation = new QPropertyAnimation(this, "opacity", this);
    m_hoverAnimation->setDuration(200);
    m_hoverAnimation->setEasingCurve(QEasingCurve::InOutQuad);
    
    // Setup shadow effect
    m_shadowEffect = new QGraphicsDropShadowEffect(this);
    m_shadowEffect->setBlurRadius(15);
    m_shadowEffect->setColor(QColor(0, 0, 0, 80));
    m_shadowEffect->setOffset(0, 4);
    setGraphicsEffect(m_shadowEffect);
}

AppCardWidget::~AppCardWidget() {
}

void AppCardWidget::setOpacity(qreal opacity) {
    m_opacity = opacity;
    update();
}

void AppCardWidget::setupUI() {
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(10);
    mainLayout->setContentsMargins(15, 15, 15, 15);
    
    // Icon
    m_iconLabel = new QLabel(this);
    m_iconLabel->setFixedSize(ICON_SIZE, ICON_SIZE);
    m_iconLabel->setAlignment(Qt::AlignCenter);
    m_iconLabel->setScaledContents(false);
    
    if (!m_appInfo.icon.isNull()) {
        QPixmap icon = m_appInfo.icon.scaled(ICON_SIZE, ICON_SIZE, 
                                             Qt::KeepAspectRatio, 
                                             Qt::SmoothTransformation);
        m_iconLabel->setPixmap(icon);
    } else {
        // Default icon placeholder
        QPixmap placeholder(ICON_SIZE, ICON_SIZE);
        placeholder.fill(QColor(60, 60, 80));
        QPainter painter(&placeholder);
        painter.setPen(QColor(150, 150, 150));
        painter.setFont(QFont("Arial", 24, QFont::Bold));
        QString initialSource = !m_appInfo.displayName.isEmpty() ? m_appInfo.displayName : m_appInfo.name;
        QString initial = initialSource.isEmpty() ? "?" : initialSource.left(1).toUpper();
        painter.drawText(placeholder.rect(), Qt::AlignCenter, initial);
        m_iconLabel->setPixmap(placeholder);

        // TEMPORARILY DISABLED: Icon loading causes memory issues
        // Just use placeholder for now
        // QTimer::singleShot(0, this, [this]() {
        //     AppStreamMetadata* metadata = new AppStreamMetadata(this);
        //     QString iconLookupName = !m_appInfo.packageName.isEmpty() ? m_appInfo.packageName : m_appInfo.name;
        //     QPixmap icon = metadata->findIconForPackage(iconLookupName, ICON_SIZE);
        //     if (icon.isNull()) {
        //         icon = metadata->getIcon(iconLookupName, ICON_SIZE);
        //     }
        //     if (!icon.isNull()) {
        //         m_iconLabel->setPixmap(icon.scaled(ICON_SIZE, ICON_SIZE, 
        //                                            Qt::KeepAspectRatio, 
        //                                            Qt::SmoothTransformation));
        //     }
        //     metadata->deleteLater();
        // });
    }
    
    mainLayout->addWidget(m_iconLabel, 0, Qt::AlignHCenter);
    
    // Name (with proper word wrap and ellipsis)
    m_nameLabel = new QLabel(this);
    QString displayName = !m_appInfo.displayName.isEmpty() ? m_appInfo.displayName : m_appInfo.name;
    
    // Debug: log the actual name being displayed using fprintf
    fprintf(stderr, "AppCardWidget: pkgName='%s' dispName='%s' name='%s' final='%s'\n",
            m_appInfo.packageName.toUtf8().constData(),
            m_appInfo.displayName.toUtf8().constData(),
            m_appInfo.name.toUtf8().constData(),
            displayName.toUtf8().constData());
    // Also log hex of displayName
    fprintf(stderr, "  displayName hex: ");
    for (int i = 0; i < displayName.length() && i < 20; i++) {
        fprintf(stderr, "%04x ", displayName[i].unicode());
    }
    fprintf(stderr, "\n");
    fflush(stderr);
    
    // Truncate very long names
    if (displayName.length() > 30) {
        displayName = displayName.left(27) + "...";
    }
    m_nameLabel->setText(displayName);
    m_nameLabel->setAlignment(Qt::AlignCenter);
    m_nameLabel->setWordWrap(true);
    m_nameLabel->setTextFormat(Qt::PlainText);
    m_nameLabel->setMaximumHeight(50); // Limit height
    m_nameLabel->setStyleSheet(
        "QLabel {"
        "color: #ffffff;"
        "font-size: 15px;"
        "font-weight: bold;"
        "background: transparent;"
        "line-height: 1.2;"
        "}"
    );
    mainLayout->addWidget(m_nameLabel, 0, Qt::AlignHCenter);
    
    // Version
    m_versionLabel = new QLabel(this);
    m_versionLabel->setText(m_appInfo.version);
    m_versionLabel->setAlignment(Qt::AlignCenter);
    m_versionLabel->setStyleSheet(
        "QLabel {"
        "color: #a0a0a0;"
        "font-size: 12px;"
        "background: transparent;"
        "}"
    );
    mainLayout->addWidget(m_versionLabel);
    
    // Description (truncated, with proper word wrap)
    m_descriptionLabel = new QLabel(this);
    QString desc = m_appInfo.description;
    if (desc.length() > 80) {
        desc = desc.left(77) + "...";
    }
    m_descriptionLabel->setText(desc);
    m_descriptionLabel->setAlignment(Qt::AlignCenter | Qt::AlignTop);
    m_descriptionLabel->setWordWrap(true);
    m_descriptionLabel->setTextFormat(Qt::PlainText);
    m_descriptionLabel->setMaximumHeight(60); // Limit height to prevent overlap
    m_descriptionLabel->setStyleSheet(
        "QLabel {"
        "color: #c0c0c0;"
        "font-size: 11px;"
        "background: transparent;"
        "line-height: 1.3;"
        "}"
    );
    mainLayout->addWidget(m_descriptionLabel, 0, Qt::AlignHCenter);
    
    // Category badge
    if (!m_appInfo.categories.isEmpty()) {
        QString category = AppStreamMetadata::categoryDisplayName(m_appInfo.categories.first());
        m_categoryLabel = new QLabel(this);
        m_categoryLabel->setText(category);
        m_categoryLabel->setAlignment(Qt::AlignCenter);
        m_categoryLabel->setStyleSheet(
            "QLabel {"
            "color: #6c5ce7;"
            "font-size: 10px;"
            "font-weight: 500;"
            "background: rgba(108, 92, 231, 0.2);"
            "border-radius: 8px;"
            "padding: 4px 8px;"
            "}"
        );
        mainLayout->addWidget(m_categoryLabel);
    } else {
        m_categoryLabel = nullptr;
    }
    
    // Size
    m_sizeLabel = new QLabel(this);
    m_sizeLabel->setText(m_appInfo.sizeFormatted());
    m_sizeLabel->setAlignment(Qt::AlignCenter);
    m_sizeLabel->setStyleSheet(
        "QLabel {"
        "color: #888888;"
        "font-size: 11px;"
        "background: transparent;"
        "}"
    );
    mainLayout->addWidget(m_sizeLabel);
    
    mainLayout->addStretch();
    
    // Download button
    m_downloadButton = new QPushButton("Download & Convert", this);
    m_downloadButton->setStyleSheet(
        "QPushButton {"
        "background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #6c5ce7, stop:1 #4a90e2);"
        "color: white;"
        "font-weight: bold;"
        "border-radius: 8px;"
        "padding: 10px;"
        "border: none;"
        "}"
        "QPushButton:hover {"
        "background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #7d6ff0, stop:1 #5ba0f2);"
        "}"
        "QPushButton:pressed {"
        "background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #5b4dd0, stop:1 #3a70c2);"
        "}"
    );
    connect(m_downloadButton, &QPushButton::clicked, this, &AppCardWidget::onDownloadButtonClicked);
    mainLayout->addWidget(m_downloadButton);
}

void AppCardWidget::updateCardStyle() {
    QString style = QString(
        "AppCardWidget {"
        "background: qlineargradient(x1:0, y1:0, x2:0, y2:1, "
        "stop:0 rgba(40, 40, 60, %1), stop:1 rgba(30, 30, 50, %1));"
        "border: 2px solid rgba(108, 92, 231, %2);"
        "border-radius: 12px;"
        "}"
    ).arg(m_opacity).arg(m_hovered ? 0.6 : 0.3);
    
    setStyleSheet(style);
}

void AppCardWidget::enterEvent(QEnterEvent* event) {
    Q_UNUSED(event)
    m_hovered = true;
    m_hoverAnimation->stop();
    m_hoverAnimation->setStartValue(m_opacity);
    m_hoverAnimation->setEndValue(0.95);
    m_hoverAnimation->start();
    updateCardStyle();
    
    // Increase shadow on hover
    m_shadowEffect->setBlurRadius(20);
    m_shadowEffect->setColor(QColor(108, 92, 231, 100));
}

void AppCardWidget::leaveEvent(QEvent* event) {
    Q_UNUSED(event)
    m_hovered = false;
    m_hoverAnimation->stop();
    m_hoverAnimation->setStartValue(m_opacity);
    m_hoverAnimation->setEndValue(1.0);
    m_hoverAnimation->start();
    updateCardStyle();
    
    // Restore shadow
    m_shadowEffect->setBlurRadius(15);
    m_shadowEffect->setColor(QColor(0, 0, 0, 80));
}

void AppCardWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        // Only emit if clicking on the card itself, not on button
        if (!m_downloadButton->geometry().contains(event->pos())) {
            onCardClicked();
        }
    }
    QWidget::mousePressEvent(event);
}

void AppCardWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    
    // Draw rounded rectangle background
    QRect rect = this->rect().adjusted(1, 1, -1, -1);
    QPainterPath path;
    path.addRoundedRect(rect, 12, 12);
    
    QColor bgColor(40, 40, 60);
    bgColor.setAlphaF(m_opacity);
    painter.fillPath(path, bgColor);
    
    QColor borderColor(108, 92, 231);
    borderColor.setAlphaF(m_hovered ? 0.6 * m_opacity : 0.3 * m_opacity);
    painter.setPen(QPen(borderColor, 2));
    painter.drawPath(path);
}

void AppCardWidget::onDownloadButtonClicked() {
    emit downloadClicked(m_appInfo);
}

void AppCardWidget::onCardClicked() {
    emit detailsClicked(m_appInfo);
}

