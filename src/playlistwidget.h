#ifndef PLAYLISTWIDGET_H
#define PLAYLISTWIDGET_H

#include <QWidget>
#include <QListWidget>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSet>
#include <QMap>
#include <memory>
#include "playlist.h"
#include "deezerapi.h"

class PlaylistWidget : public QWidget
{
    Q_OBJECT

public:
    explicit PlaylistWidget(QWidget *parent = nullptr);

    void setDeezerAPI(DeezerAPI* api);
    void setPlaylists(const QList<std::shared_ptr<Playlist>>& playlists);
    void setCurrentPlaylist(std::shared_ptr<Playlist> playlist);

signals:
    void playlistSelected(std::shared_ptr<Playlist> playlist);
    void playlistDoubleClicked(std::shared_ptr<Playlist> playlist);
    void debugLog(const QString& message);

private slots:
    void onLoadPlaylistsClicked();
    void onPlaylistsReceived(QList<std::shared_ptr<Playlist>> playlists);
    void onPlaylistItemClicked(QListWidgetItem* item);
    void onPlaylistItemDoubleClicked(QListWidgetItem* item);
    void onSearchTextChanged(const QString& text);
    void onImageDownloaded(QNetworkReply* reply);
    void loadVisiblePlaylistCovers();

private:
    void setupUI();
    void populateList();
    void filterPlaylists(const QString& searchText);
    void loadPlaylistCover(const QString& url, QListWidgetItem* item);

    DeezerAPI* m_deezerAPI;
    QList<std::shared_ptr<Playlist>> m_playlists;
    QList<std::shared_ptr<Playlist>> m_filteredPlaylists;

    QLabel* m_titleLabel;
    QLineEdit* m_searchEdit;
    QPushButton* m_loadButton;
    QListWidget* m_playlistList;

    QNetworkAccessManager* m_imageLoader;
    QMap<QNetworkReply*, QListWidgetItem*> m_pendingImages;
    QSet<QListWidgetItem*> m_loadedItems;
};

#endif // PLAYLISTWIDGET_H
