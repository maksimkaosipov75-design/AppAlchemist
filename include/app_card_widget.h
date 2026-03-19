#ifndef APP_CARD_WIDGET_H
#define APP_CARD_WIDGET_H

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPixmap>
#include <QPropertyAnimation>
#include <QGraphicsDropShadowEffect>
#include <QEnterEvent>
#include "appstream_metadata.h"

class AppCardWidget : public QWidget {
    Q_OBJECT
    Q_PROPERTY(qreal opacity READ opacity WRITE setOpacity)

public:
    explicit AppCardWidget(const AppInfo& appInfo, QWidget* parent = nullptr);
    ~AppCardWidget();
    
    AppInfo appInfo() const { return m_appInfo; }
    
    qreal opacity() const { return m_opacity; }
    void setOpacity(qreal opacity);

signals:
    void downloadClicked(const AppInfo& appInfo);
    void detailsClicked(const AppInfo& appInfo);

protected:
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private slots:
    void onDownloadButtonClicked();
    void onCardClicked();

private:
    void setupUI();
    void updateCardStyle();
    
    AppInfo m_appInfo;
    
    QLabel* m_iconLabel;
    QLabel* m_nameLabel;
    QLabel* m_versionLabel;
    QLabel* m_descriptionLabel;
    QLabel* m_sizeLabel;
    QLabel* m_categoryLabel;
    QPushButton* m_downloadButton;
    
    QPropertyAnimation* m_hoverAnimation;
    QGraphicsDropShadowEffect* m_shadowEffect;
    
    qreal m_opacity;
    bool m_hovered;
    
    static const int CARD_WIDTH = 280;
    static const int CARD_HEIGHT = 320;
    static const int ICON_SIZE = 96;
};

#endif // APP_CARD_WIDGET_H

