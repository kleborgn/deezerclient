#ifndef SEARCHWIDGET_H
#define SEARCHWIDGET_H

#include <QWidget>
#include <QLineEdit>
#include <QTabWidget>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QMap>
#include <QSet>
#include <memory>
#include "deezerapi.h"
#include "track.h"
#include "album.h"
#include "playlist.h"

class SearchWidget : public QWidget
{
    Q_OBJECT

public:
    explicit SearchWidget(QWidget *parent = nullptr);

    void setDeezerAPI(DeezerAPI* api);

signals:
    void trackDoubleClicked(std::shared_ptr<Track> track);
    void albumDoubleClicked(std::shared_ptr<Album> album);
    void playlistDoubleClicked(std::shared_ptr<Playlist> playlist);
    void tracksSelected(QList<std::shared_ptr<Track>> tracks);
    void debugLog(const QString& message);
    void addToQueueRequested(QList<std::shared_ptr<Track>> tracks);
    void playNextRequested(QList<std::shared_ptr<Track>> tracks);

private slots:
    void onSearchTriggered();
    void onTabChanged(int index);
    void onTracksFound(QList<std::shared_ptr<Track>> tracks, void* sender = nullptr);
    void onAlbumsFound(QList<std::shared_ptr<Album>> albums, void* sender = nullptr);
    void onPlaylistsFound(QList<std::shared_ptr<Playlist>> playlists);
    void onTrackItemDoubleClicked(QListWidgetItem* item);
    void onAlbumItemDoubleClicked(QListWidgetItem* item);
    void onPlaylistItemDoubleClicked(QListWidgetItem* item);
    void onImageDownloaded(QNetworkReply* reply);
    void onTrackScrolled();
    void onAlbumScrolled();
    void onPlaylistScrolled();
    void onTracksContextMenu(const QPoint& pos);

private:
    void setupUI();
    void loadVisibleTrackArt();
    void loadVisibleAlbumArt();
    void loadVisiblePlaylistCovers();
    void loadImage(const QString& url, QListWidgetItem* item);

    DeezerAPI* m_deezerAPI;  // Shared API for all operations

    QLineEdit* m_searchEdit;
    QPushButton* m_searchButton;
    QTabWidget* m_resultsTabWidget;

    QListWidget* m_tracksList;
    QListWidget* m_albumsList;
    QListWidget* m_playlistsList;

    QList<std::shared_ptr<Track>> m_tracks;
    QList<std::shared_ptr<Album>> m_albums;
    QList<std::shared_ptr<Playlist>> m_playlists;

    QNetworkAccessManager* m_imageLoader;
    QMap<QNetworkReply*, QListWidgetItem*> m_pendingImages;
    QSet<QListWidgetItem*> m_loadedTrackItems;
    QSet<QListWidgetItem*> m_loadedAlbumItems;
    QSet<QListWidgetItem*> m_loadedPlaylistItems;
};

#endif // SEARCHWIDGET_H
