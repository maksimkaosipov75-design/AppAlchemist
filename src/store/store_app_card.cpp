#include "store/store_app_card.h"
#include <QRegularExpression>
#include "store/name_trace.h"

namespace {
QString ratingText(double rating, int count) {
    if (count == 0) {
        return "No ratings yet";
    }
    return QString("Rating %1 (%2)").arg(rating, 0, 'f', 1).arg(count);
}
}

StoreAppCard::StoreAppCard(const StoreAppEntry& app, const QPixmap& icon, QWidget* parent)
    : QWidget(parent)
    , m_app(app) {
    StoreNameTrace::trace("ui-card", app.packageName, app.displayName);
    setFixedSize(220, 320);
    setStyleSheet(
        "QWidget {"
        "background: rgba(30, 30, 50, 0.9);"
        "border: 1px solid rgba(108, 92, 231, 0.4);"
        "border-radius: 12px;"
        "}"
        "QWidget:hover {"
        "border: 1px solid rgba(108, 92, 231, 0.8);"
        "background: rgba(40, 40, 60, 0.95);"
        "}"
    );

    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(12, 12, 12, 12);
    mainLayout->setSpacing(8);

    m_iconLabel = new QLabel(this);
    m_iconLabel->setFixedSize(96, 96);
    m_iconLabel->setAlignment(Qt::AlignCenter);
    m_iconLabel->setStyleSheet("background: rgba(255,255,255,0.1); border-radius: 12px;");
    if (!icon.isNull()) {
        m_iconLabel->setPixmap(icon.scaled(80, 80, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    } else {
        m_iconLabel->setText("?");
        m_iconLabel->setStyleSheet("background: rgba(255,255,255,0.1); color: #888888; font-size: 32px; border-radius: 12px;");
    }
    mainLayout->addWidget(m_iconLabel, 0, Qt::AlignHCenter);
    mainLayout->addSpacing(4);

    // Work with bytes to avoid QString corruption
    QByteArray nameBytes = app.effectiveName().toUtf8();
    QByteArray cleanBytes;
    cleanBytes.reserve(nameBytes.size());
    
    // Remove control characters at byte level
    for (int i = 0; i < nameBytes.size(); ++i) {
        unsigned char ch = nameBytes.at(i);
        if (ch >= 32 || (ch & 0x80)) {  // Keep printable ASCII and UTF-8
            cleanBytes.append(ch);
        }
    }
    
    QString appName = QString::fromUtf8(cleanBytes);
    m_nameLabel = new QLabel(appName, this);
    m_nameLabel->setAlignment(Qt::AlignCenter);
    m_nameLabel->setWordWrap(true);
    m_nameLabel->setMaximumHeight(40);
    m_nameLabel->setTextFormat(Qt::PlainText);
    m_nameLabel->setStyleSheet(
        "color: #ffffff;"
        "font-weight: 600;"
        "font-size: 15px;"
        "font-family: 'Segoe UI', 'DejaVu Sans', 'Liberation Sans', sans-serif;"
    );
    mainLayout->addWidget(m_nameLabel);

    // Work with bytes to avoid QString corruption
    QByteArray summaryBytes = app.summary.toUtf8();
    QByteArray cleanSummary;
    cleanSummary.reserve(summaryBytes.size());
    
    for (int i = 0; i < summaryBytes.size(); ++i) {
        unsigned char ch = summaryBytes.at(i);
        if (ch >= 32 || (ch & 0x80)) {
            cleanSummary.append(ch);
        }
    }
    
    QString summary = QString::fromUtf8(cleanSummary);
    m_summaryLabel = new QLabel(summary, this);
    m_summaryLabel->setAlignment(Qt::AlignCenter);
    m_summaryLabel->setWordWrap(true);
    m_summaryLabel->setMaximumHeight(40);
    m_summaryLabel->setTextFormat(Qt::PlainText);
    m_summaryLabel->setStyleSheet(
        "color: #b0b0b0;"
        "font-size: 12px;"
        "font-family: 'Segoe UI', 'DejaVu Sans', 'Liberation Sans', sans-serif;"
    );
    mainLayout->addWidget(m_summaryLabel);

    m_ratingLabel = new QLabel(ratingText(app.ratingAverage, app.ratingCount), this);
    m_ratingLabel->setAlignment(Qt::AlignCenter);
    m_ratingLabel->setTextFormat(Qt::PlainText);
    m_ratingLabel->setStyleSheet("color: #888888; font-size: 11px;");
    mainLayout->addWidget(m_ratingLabel);
    mainLayout->addSpacing(8);

    QHBoxLayout* buttonLayout = new QHBoxLayout();
    buttonLayout->setSpacing(8);
    m_detailsButton = new QPushButton("Details", this);
    m_downloadButton = new QPushButton("Install", this);
    m_detailsButton->setStyleSheet(
        "QPushButton {"
        "background: rgba(108, 92, 231, 0.4);"
        "color: #e0e0e0;"
        "border: 1px solid rgba(108, 92, 231, 0.6);"
        "border-radius: 8px;"
        "padding: 8px;"
        "font-weight: 500;"
        "font-size: 13px;"
        "}"
        "QPushButton:hover {"
        "background: rgba(108, 92, 231, 0.6);"
        "border: 1px solid #6c5ce7;"
        "}"
    );
    m_downloadButton->setStyleSheet(
        "QPushButton {"
        "background: rgba(108, 92, 231, 0.7);"
        "color: white;"
        "border: none;"
        "border-radius: 8px;"
        "padding: 8px;"
        "font-weight: 500;"
        "font-size: 13px;"
        "}"
        "QPushButton:hover {"
        "background: rgba(108, 92, 231, 0.9);"
        "}"
        "QPushButton:pressed {"
        "background: rgba(108, 92, 231, 1.0);"
        "}"
    );
    buttonLayout->addWidget(m_detailsButton);
    buttonLayout->addWidget(m_downloadButton);
    mainLayout->addLayout(buttonLayout);

    connect(m_detailsButton, &QPushButton::clicked, this, [this]() {
        emit detailsRequested(m_app);
    });
    connect(m_downloadButton, &QPushButton::clicked, this, [this]() {
        emit downloadRequested(m_app);
    });
}

void StoreAppCard::setIcon(const QPixmap& icon) {
    if (!icon.isNull()) {
        m_iconLabel->setPixmap(icon.scaled(80, 80, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        m_iconLabel->setStyleSheet("background: rgba(255,255,255,0.1); border-radius: 12px;");
    }
}

