#ifndef QUEUEHEADERWIDGET_H
#define QUEUEHEADERWIDGET_H

#include <QWidget>
#include <QLabel>
#include <memory>
#include "album.h"
#include "playlist.h"

class QNetworkAccessManager;

class QueueHeaderWidget : public QWidget
{
    Q_OBJECT

public:
    explicit QueueHeaderWidget(QWidget *parent = nullptr);

    void setAlbum(std::shared_ptr<Album> album);
    void setPlaylist(std::shared_ptr<Playlist> playlist);
    void setAlbumScrobbleCount(int count);
    void setStreamInfo(const QString& info);
    void clear();

private:
    void setupUI();
    QString formatDuration(int seconds);
    void loadImage(const QString& url);
    void updateStatsLine();

    QLabel* m_artLabel;
    QLabel* m_artistLabel;
    QLabel* m_titleLabel;
    QLabel* m_statsLabel;
    QLabel* m_streamInfoLabel;
    QLabel* m_scrobbleLabel;

    QString m_baseStats;  // "12 Tracks | Time: 36:49"

    QNetworkAccessManager* m_networkManager;
};

#endif // QUEUEHEADERWIDGET_H
