#include "albumlistwidget.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QNetworkRequest>
#include <QScrollBar>
#include <QDebug>
#include <QTimer>

AlbumListWidget::AlbumListWidget(QWidget *parent)
    : QWidget(parent)
    , m_deezerAPI(nullptr)
    , m_imageLoader(new QNetworkAccessManager(this))
    , m_searchDelayTimer(new QTimer(this))
{
    setupUI();
    connect(m_imageLoader, &QNetworkAccessManager::finished,
            this, &AlbumListWidget::onImageDownloaded);

    // Setup search debounce timer
    m_searchDelayTimer->setSingleShot(true);
    m_searchDelayTimer->setInterval(300);  // 300ms delay
    connect(m_searchDelayTimer, &QTimer::timeout, this, &AlbumListWidget::onSearchTriggered);
}

void AlbumListWidget::setupUI()
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    
    // Title
    QLabel* titleLabel = new QLabel("Albums", this);
    QFont titleFont = titleLabel->font();
    titleFont.setPointSize(14);
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);
    mainLayout->addWidget(titleLabel);
    
    // Search Bar
    QHBoxLayout* searchLayout = new QHBoxLayout();
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText("Search albums...");
    searchLayout->addWidget(m_searchEdit);
    
    QPushButton* searchButton = new QPushButton("Search", this);
    searchLayout->addWidget(searchButton);
    mainLayout->addLayout(searchLayout);
    
    // Load Favorites Button
    m_loadFavoritesButton = new QPushButton("My Favorite Albums", this);
    mainLayout->addWidget(m_loadFavoritesButton);
    
    // Album List
    m_albumList = new QListWidget(this);
    m_albumList->setStyleSheet("QListWidget::item { padding: 5px; }");
    m_albumList->setIconSize(QSize(64, 64));  // Set icon size for album art
    mainLayout->addWidget(m_albumList);
    
    // Connect signals
    connect(searchButton, &QPushButton::clicked, this, &AlbumListWidget::onSearchTriggered);
    // Debounce text changes - restart timer on each keystroke
    connect(m_searchEdit, &QLineEdit::textChanged, this, [this]() {
        m_searchDelayTimer->start();  // Restart timer (cancels previous if still running)
    });
    connect(m_loadFavoritesButton, &QPushButton::clicked, this, &AlbumListWidget::onLoadFavoritesClicked);
    connect(m_albumList, &QListWidget::itemClicked, this, &AlbumListWidget::onAlbumItemClicked);
    connect(m_albumList, &QListWidget::itemDoubleClicked, this, &AlbumListWidget::onAlbumItemDoubleClicked);

    // Connect to scroll events for lazy loading album art
    connect(m_albumList->verticalScrollBar(), &QScrollBar::valueChanged,
            this, &AlbumListWidget::loadVisibleAlbumArt);
}

void AlbumListWidget::setDeezerAPI(DeezerAPI* api)
{
    m_deezerAPI = api;

    if (m_deezerAPI) {
        // Safe to connect to albumsFound now that SearchWidget uses context-aware searchAlbumsFound
        connect(m_deezerAPI, &DeezerAPI::albumsFound, this, &AlbumListWidget::onAlbumsReceived);
    }
}

void AlbumListWidget::setAlbums(const QList<std::shared_ptr<Album>>& albums)
{
    m_albums = albums;
    populateList();
}

void AlbumListWidget::onLoadFavoritesClicked()
{
    if (m_deezerAPI) {
        m_deezerAPI->getUserAlbums();
    }
}

void AlbumListWidget::onAlbumsReceived(QList<std::shared_ptr<Album>> albums)
{
    // If we receive albums from loading favorites, store them
    // Otherwise, it's from search and we just display them
    if (!albums.isEmpty() && m_albums.isEmpty()) {
        m_favoriteAlbums = albums;
    }
    m_albums = albums;
    populateList();
}

void AlbumListWidget::onSearchTriggered()
{
    QString query = m_searchEdit->text().trimmed();
    
    // If we have favorite albums loaded, search locally through them
    if (!query.isEmpty() && !m_favoriteAlbums.isEmpty()) {
        QList<std::shared_ptr<Album>> filteredAlbums;
        QString lowerQuery = query.toLower();
        
        for (const auto& album : m_favoriteAlbums) {
            // Search in album title and artist name
            if (album->title().toLower().contains(lowerQuery) ||
                album->artist().toLower().contains(lowerQuery)) {
                filteredAlbums.append(album);
            }
        }
        
        setAlbums(filteredAlbums);
    } else if (query.isEmpty()) {
        // If search is empty, show all favorite albums if available
        if (!m_favoriteAlbums.isEmpty()) {
            setAlbums(m_favoriteAlbums);
        }
    }
}

void AlbumListWidget::onAlbumItemClicked(QListWidgetItem* item)
{
    int index = m_albumList->row(item);
    if (index >= 0 && index < m_albums.size()) {
        emit albumSelected(m_albums[index]);
    }
}

void AlbumListWidget::onAlbumItemDoubleClicked(QListWidgetItem* item)
{
    int index = m_albumList->row(item);
    if (index >= 0 && index < m_albums.size()) {
        emit albumDoubleClicked(m_albums[index]);
    }
}

void AlbumListWidget::cancelPendingImageLoads()
{
    // Abort all pending network requests
    for (QNetworkReply* reply : m_pendingImages.keys()) {
        reply->abort();
        reply->deleteLater();
    }
    m_pendingImages.clear();
}

void AlbumListWidget::applyCachedImages()
{
    // Apply cached images to all items after layout is complete
    for (int i = 0; i < m_albums.size(); ++i) {
        QListWidgetItem* item = m_albumList->item(i);
        if (!item) continue;
        
        const auto& album = m_albums[i];
        if (m_imageCache.contains(album->id())) {
            item->setIcon(QIcon(m_imageCache[album->id()]));
        }
    }
}

void AlbumListWidget::populateList()
{
    cancelPendingImageLoads();  // Cancel any pending loads before clearing
    m_albumList->clear();

    emit debugLog(QString("[AlbumListWidget] Populating %1 albums").arg(m_albums.size()));

    for (const auto& album : m_albums) {
        QString text = QString("%1\n%2")
                          .arg(album->title())
                          .arg(album->artist());

        QListWidgetItem* item = new QListWidgetItem(text, m_albumList);
        item->setSizeHint(QSize(item->sizeHint().width(), 74));  // Set height to accommodate image

        // Set a placeholder icon initially
        QPixmap placeholder(64, 64);
        placeholder.fill(Qt::lightGray);
        item->setIcon(QIcon(placeholder));

        m_albumList->addItem(item);
    }

    // Force a layout update to ensure items are properly positioned
    m_albumList->updateGeometry();
    m_albumList->viewport()->update();
    
    // Apply cached images after layout is complete
    QTimer::singleShot(50, this, [this]() {
        applyCachedImages();
    });
    
    // Load album art for visible items after a short delay to ensure layout is complete
    QTimer::singleShot(100, this, [this]() {
        loadVisibleAlbumArt();
    });
}

void AlbumListWidget::loadVisibleAlbumArt()
{
    // Get visible area
    QRect visibleRect = m_albumList->viewport()->rect();

    // Find first and last visible items
    QListWidgetItem* topItem = m_albumList->itemAt(visibleRect.topLeft());
    QListWidgetItem* bottomItem = m_albumList->itemAt(visibleRect.bottomLeft());

    int firstVisible = 0;
    int lastVisible = m_albums.size() - 1;

    // If we found visible items, use them; otherwise load all items
    if (topItem && bottomItem) {
        firstVisible = m_albumList->row(topItem);
        lastVisible = m_albumList->row(bottomItem);

        // Load a few items above and below for smooth scrolling
        int bufferSize = 5;
        firstVisible = qMax(0, firstVisible - bufferSize);
        lastVisible = qMin(m_albums.size() - 1, lastVisible + bufferSize);
    } else if (topItem) {
        // Only top is visible, load from top downward
        firstVisible = m_albumList->row(topItem);
        lastVisible = qMin(m_albums.size() - 1, firstVisible + 10);
    } else if (bottomItem) {
        // Only bottom is visible, load upward
        lastVisible = m_albumList->row(bottomItem);
        firstVisible = qMax(0, lastVisible - 10);
    } else {
        // Can't determine visibility, load first batch
        lastVisible = qMin(19, m_albums.size() - 1);  // Load first 20 items
    }

    // Load album art for visible items
    for (int i = firstVisible; i <= lastVisible; ++i) {
        if (i >= 0 && i < m_albums.size()) {
            QListWidgetItem* item = m_albumList->item(i);
            const auto& album = m_albums[i];
            
            // Skip if already cached or if there's no cover URL
            if (album->coverUrl().isEmpty() || m_imageCache.contains(album->id())) {
                continue;
            }

            // Check if we're already loading this album's art
            bool alreadyLoading = false;
            for (QNetworkReply* reply : m_pendingImages.keys()) {
                if (reply->url().toString() == album->coverUrl()) {
                    alreadyLoading = true;
                    break;
                }
            }
            
            if (alreadyLoading) {
                continue;
            }

            loadAlbumArt(album->coverUrl(), album->id(), item);
        }
    }
}

void AlbumListWidget::loadAlbumArt(const QString& url, const QString& albumId, QListWidgetItem* item)
{
    QNetworkRequest request(url);
    QNetworkReply* reply = m_imageLoader->get(request);
    m_pendingImages[reply] = qMakePair(item, albumId);
}

void AlbumListWidget::onImageDownloaded(QNetworkReply* reply)
{
    if (!reply) {
        emit debugLog("[AlbumListWidget] onImageDownloaded: reply is null");
        return;
    }

    if (!m_pendingImages.contains(reply)) {
        emit debugLog("[AlbumListWidget] onImageDownloaded: reply not in pending images");
        reply->deleteLater();
        return;
    }

    QPair<QListWidgetItem*, QString> pair = m_pendingImages.take(reply);
    QListWidgetItem* item = pair.first;
    QString albumId = pair.second;
    QString url = reply->url().toString();

    if (reply->error() == QNetworkReply::NoError) {
        QByteArray imageData = reply->readAll();

        QPixmap pixmap;
        if (pixmap.loadFromData(imageData)) {
            // Scale to fit icon size while maintaining aspect ratio
            QPixmap scaled = pixmap.scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            
            // Cache the scaled image by album ID
            m_imageCache[albumId] = scaled;
            
            // Update the item if it still exists in the list
            if (item && m_albumList->row(item) >= 0) {
                item->setIcon(QIcon(scaled));
            }
        } else {
            emit debugLog("[AlbumListWidget] Failed to load pixmap from data");
        }
    } else {
        emit debugLog(QString("[AlbumListWidget] Failed to download album art from %1: %2")
                     .arg(url, reply->errorString()));
    }

    reply->deleteLater();
}
