#ifndef STORE_APP_CARD_H
#define STORE_APP_CARD_H

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include "store_appentry.h"

class StoreAppCard : public QWidget {
    Q_OBJECT

public:
    explicit StoreAppCard(const StoreAppEntry& app, const QPixmap& icon, QWidget* parent = nullptr);
    void setIcon(const QPixmap& icon);

signals:
    void detailsRequested(const StoreAppEntry& app);
    void downloadRequested(const StoreAppEntry& app);

private:
    StoreAppEntry m_app;
    QLabel* m_iconLabel;
    QLabel* m_nameLabel;
    QLabel* m_summaryLabel;
    QLabel* m_ratingLabel;
    QPushButton* m_detailsButton;
    QPushButton* m_downloadButton;
};

#endif // STORE_APP_CARD_H

