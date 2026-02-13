#ifndef ALBUMLISTWIDGET_H
#define ALBUMLISTWIDGET_H

#include <QWidget>
#include <QListWidget>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPixmap>
#include <QMap>
#include <QSet>
#include <QTimer>
#include <memory>
#include "album.h"
#include "deezerapi.h"

class AlbumListWidget : public QWidget
{
    Q_OBJECT

public:
    explicit AlbumListWidget(QWidget *parent = nullptr);

    void setDeezerAPI(DeezerAPI* api);
    void setAlbums(const QList<std::shared_ptr<Album>>& albums);

signals:
    void albumSelected(std::shared_ptr<Album> album);
    void albumDoubleClicked(std::shared_ptr<Album> album);
    void debugLog(const QString& message);

private slots:
    void onLoadFavoritesClicked();
    void onSearchTriggered();
    void onAlbumsReceived(QList<std::shared_ptr<Album>> albums);
    void onAlbumItemClicked(QListWidgetItem* item);
    void onAlbumItemDoubleClicked(QListWidgetItem* item);
    void onImageDownloaded(QNetworkReply* reply);
    void loadVisibleAlbumArt();
    void applyCachedImages();

private:
    void setupUI();
    void populateList();
    void loadAlbumArt(const QString& url, const QString& albumId, QListWidgetItem* item);
    void cancelPendingImageLoads();

    DeezerAPI* m_deezerAPI;
    QList<std::shared_ptr<Album>> m_albums;
    QList<std::shared_ptr<Album>> m_favoriteAlbums;  // Store favorite albums for local search

    QLineEdit* m_searchEdit;
    QPushButton* m_loadFavoritesButton;
    QListWidget* m_albumList;
    QNetworkAccessManager* m_imageLoader;
    QMap<QNetworkReply*, QPair<QListWidgetItem*, QString>> m_pendingImages;  // reply -> (item, albumId)
    QMap<QString, QPixmap> m_imageCache;  // Cache loaded album art by album ID
    QTimer* m_searchDelayTimer;  // Debounce timer for search
};

#endif // ALBUMLISTWIDGET_H
