#ifndef RECENTWIDGET_H
#define RECENTWIDGET_H

#include <QWidget>
#include <QJsonArray>
#include <QMap>
#include <QVector>
#include <QPixmap>

class QListWidget;
class QListWidgetItem;
class QNetworkAccessManager;
class QNetworkReply;
class DeezerAPI;

class RecentWidget : public QWidget
{
    Q_OBJECT

public:
    explicit RecentWidget(QWidget* parent = nullptr);

    void setDeezerAPI(DeezerAPI* api);
    void refresh();

signals:
    void albumDoubleClicked(const QString& albumId);
    void playlistDoubleClicked(const QString& playlistId);
    void debugLog(const QString& message);

protected:
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void onRecentlyPlayedReceived(const QJsonArray& items);
    void onItemDoubleClicked(QListWidgetItem* item);
    void onImageDownloaded(QNetworkReply* reply);

private:
    QString coverUrl(const QString& type, const QString& pictureId);
    int computeCoverSize() const;
    void updateGridSize();

    DeezerAPI* m_deezerAPI = nullptr;
    QListWidget* m_listWidget;
    QNetworkAccessManager* m_imageManager;
    QMap<QNetworkReply*, int> m_pendingImages; // reply → row index
    QVector<QPixmap> m_originalPixmaps;        // full-res pixmaps for rescaling
};

#endif // RECENTWIDGET_H
