#include "store/store_details_dialog.h"
#include <QHBoxLayout>

StoreDetailsDialog::StoreDetailsDialog(const StoreAppEntry& app,
                                       const QPixmap& icon,
                                       StoreRatings* ratings,
                                       QWidget* parent)
    : QDialog(parent)
    , m_app(app)
    , m_ratings(ratings) {
    setWindowTitle(app.effectiveName());
    setMinimumSize(520, 520);
    setStyleSheet("QDialog { background: #1a1a2e; color: #e0e0e0; }");

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setSpacing(10);

    QLabel* iconLabel = new QLabel(this);
    iconLabel->setFixedSize(96, 96);
    iconLabel->setAlignment(Qt::AlignCenter);
    if (!icon.isNull()) {
        iconLabel->setPixmap(icon.scaled(80, 80, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    } else {
        iconLabel->setText("?");
        iconLabel->setStyleSheet("color: #a0a0a0; font-size: 28px;");
    }
    layout->addWidget(iconLabel, 0, Qt::AlignHCenter);

    m_titleLabel = new QLabel(app.effectiveName(), this);
    m_titleLabel->setAlignment(Qt::AlignCenter);
    m_titleLabel->setStyleSheet("color: #ffffff; font-size: 20px; font-weight: bold;");
    layout->addWidget(m_titleLabel);

    m_summaryLabel = new QLabel(app.summary, this);
    m_summaryLabel->setAlignment(Qt::AlignCenter);
    m_summaryLabel->setWordWrap(true);
    m_summaryLabel->setStyleSheet("color: #b0b0b0; font-size: 12px;");
    layout->addWidget(m_summaryLabel);

    m_description = new QTextEdit(this);
    m_description->setReadOnly(true);
    m_description->setText(app.description);
    m_description->setStyleSheet("background: rgba(0,0,0,0.3); color: #e0e0e0; border: 1px solid rgba(255,255,255,0.1);");
    layout->addWidget(m_description, 1);

    QLabel* ratingsTitle = new QLabel("Ratings & Comments", this);
    ratingsTitle->setStyleSheet("color: #7dd3fc; font-weight: bold;");
    layout->addWidget(ratingsTitle);

    m_ratingsList = new QListWidget(this);
    m_ratingsList->setStyleSheet("background: rgba(0,0,0,0.2); color: #e0e0e0;");
    layout->addWidget(m_ratingsList);

    QHBoxLayout* ratingForm = new QHBoxLayout();
    m_ratingCombo = new QComboBox(this);
    for (int i = 1; i <= 5; ++i) {
        m_ratingCombo->addItem(QString::number(i));
    }
    m_commentInput = new QTextEdit(this);
    m_commentInput->setFixedHeight(60);
    m_commentInput->setPlaceholderText("Leave a comment...");
    m_submitButton = new QPushButton("Submit", this);
    m_submitButton->setStyleSheet("QPushButton { background: rgba(108, 92, 231, 0.6); color: white; border-radius: 6px; padding: 6px 12px; }");
    ratingForm->addWidget(m_ratingCombo);
    ratingForm->addWidget(m_commentInput, 1);
    ratingForm->addWidget(m_submitButton);
    layout->addLayout(ratingForm);

    m_downloadButton = new QPushButton("Download", this);
    m_downloadButton->setStyleSheet("QPushButton { background: rgba(74, 144, 226, 0.7); color: white; border-radius: 8px; padding: 8px; }");
    layout->addWidget(m_downloadButton);

    connect(m_submitButton, &QPushButton::clicked, this, &StoreDetailsDialog::onSubmitRating);
    connect(m_downloadButton, &QPushButton::clicked, this, [this]() {
        emit downloadRequested(m_app);
    });

    refreshRatings();
}

void StoreDetailsDialog::onSubmitRating() {
    int rating = m_ratingCombo->currentText().toInt();
    QString comment = m_commentInput->toPlainText().trimmed();
    QString key = !m_app.appId.isEmpty() ? m_app.appId : m_app.packageName;
    if (m_ratings->addRating(key, rating, comment)) {
        m_commentInput->clear();
        refreshRatings();
    }
}

void StoreDetailsDialog::refreshRatings() {
    m_ratingsList->clear();
    QString key = !m_app.appId.isEmpty() ? m_app.appId : m_app.packageName;
    QList<StoreRatingEntry> entries = m_ratings->getRatings(key);
    for (const StoreRatingEntry& entry : entries) {
        QString line = QString("%1★ - %2").arg(entry.rating).arg(entry.comment);
        m_ratingsList->addItem(line);
    }
}

