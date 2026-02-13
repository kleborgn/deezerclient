#include "playlistwidget.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QNetworkRequest>
#include <QScrollBar>
#include <algorithm>

PlaylistWidget::PlaylistWidget(QWidget *parent)
    : QWidget(parent)
    , m_deezerAPI(nullptr)
    , m_imageLoader(new QNetworkAccessManager(this))
{
    setupUI();
    connect(m_imageLoader, &QNetworkAccessManager::finished,
            this, &PlaylistWidget::onImageDownloaded);
}

void PlaylistWidget::setupUI()
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);

    m_titleLabel = new QLabel("Playlists", this);
    QFont titleFont = m_titleLabel->font();
    titleFont.setPointSize(14);
    titleFont.setBold(true);
    m_titleLabel->setFont(titleFont);

    // Search bar
    QHBoxLayout* searchLayout = new QHBoxLayout();
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText("Search my playlists...");
    searchLayout->addWidget(m_searchEdit);

    m_loadButton = new QPushButton("Load My Playlists", this);
    m_playlistList = new QListWidget(this);
    m_playlistList->setStyleSheet("QListWidget::item { padding: 5px; }");
    m_playlistList->setIconSize(QSize(64, 64));  // Set icon size for playlist covers

    mainLayout->addWidget(m_titleLabel);
    mainLayout->addLayout(searchLayout);
    mainLayout->addWidget(m_loadButton);
    mainLayout->addWidget(m_playlistList);

    // Connect signals
    connect(m_loadButton, &QPushButton::clicked, this, &PlaylistWidget::onLoadPlaylistsClicked);
    connect(m_playlistList, &QListWidget::itemClicked, this, &PlaylistWidget::onPlaylistItemClicked);
    connect(m_playlistList, &QListWidget::itemDoubleClicked, this, &PlaylistWidget::onPlaylistItemDoubleClicked);
    connect(m_searchEdit, &QLineEdit::textChanged, this, &PlaylistWidget::onSearchTextChanged);

    // Connect to scroll events for lazy loading playlist covers
    connect(m_playlistList->verticalScrollBar(), &QScrollBar::valueChanged,
            this, &PlaylistWidget::loadVisiblePlaylistCovers);
}

void PlaylistWidget::setDeezerAPI(DeezerAPI* api)
{
    m_deezerAPI = api;
    
    if (m_deezerAPI) {
        connect(m_deezerAPI, &DeezerAPI::playlistsFound, this, &PlaylistWidget::onPlaylistsReceived);
    }
}

void PlaylistWidget::setPlaylists(const QList<std::shared_ptr<Playlist>>& playlists)
{
    m_playlists = playlists;
    // Sort by most recently modified first
    std::sort(m_playlists.begin(), m_playlists.end(),
              [](const std::shared_ptr<Playlist>& a, const std::shared_ptr<Playlist>& b) {
                  return a->lastModified() > b->lastModified();
              });

    // Reset search filter
    m_filteredPlaylists = m_playlists;
    populateList();
}

void PlaylistWidget::setCurrentPlaylist(std::shared_ptr<Playlist> playlist)
{
    // No longer displaying current playlist in title
    // Title remains as "Playlists"
    (void)playlist; // Suppress unused parameter warning
}

void PlaylistWidget::onLoadPlaylistsClicked()
{
    if (m_deezerAPI) {
        m_deezerAPI->getUserPlaylists();
    }
}

void PlaylistWidget::onPlaylistsReceived(QList<std::shared_ptr<Playlist>> playlists)
{
    setPlaylists(playlists);
}

void PlaylistWidget::onPlaylistItemClicked(QListWidgetItem* item)
{
    int index = m_playlistList->row(item);
    if (index >= 0 && index < m_filteredPlaylists.size()) {
        emit playlistSelected(m_filteredPlaylists[index]);
    }
}

void PlaylistWidget::onPlaylistItemDoubleClicked(QListWidgetItem* item)
{
    int index = m_playlistList->row(item);
    if (index >= 0 && index < m_filteredPlaylists.size()) {
        emit playlistDoubleClicked(m_filteredPlaylists[index]);
    }
}

void PlaylistWidget::onSearchTextChanged(const QString& text)
{
    filterPlaylists(text);
}

void PlaylistWidget::filterPlaylists(const QString& searchText)
{
    if (searchText.isEmpty()) {
        m_filteredPlaylists = m_playlists;
    } else {
        m_filteredPlaylists.clear();
        for (const auto& playlist : m_playlists) {
            if (playlist->title().contains(searchText, Qt::CaseInsensitive)) {
                m_filteredPlaylists.append(playlist);
            }
        }
    }
    populateList();
}

void PlaylistWidget::populateList()
{
    m_playlistList->clear();
    m_pendingImages.clear();  // Clear any pending downloads
    m_loadedItems.clear();    // Clear loaded items tracking

    emit debugLog(QString("[PlaylistWidget] Populating %1 playlists").arg(m_filteredPlaylists.size()));

    for (const auto& playlist : m_filteredPlaylists) {
        QString text = QString("%1\n%2 tracks")
                          .arg(playlist->title())
                          .arg(playlist->trackCount());

        QListWidgetItem* item = new QListWidgetItem(text, m_playlistList);
        item->setSizeHint(QSize(item->sizeHint().width(), 74));  // Set height to accommodate image

        // Set a placeholder icon
        QPixmap placeholder(64, 64);
        placeholder.fill(Qt::lightGray);
        item->setIcon(QIcon(placeholder));

        m_playlistList->addItem(item);
    }

    // Load playlist covers for visible items only
    loadVisiblePlaylistCovers();
}

void PlaylistWidget::loadVisiblePlaylistCovers()
{
    // Get visible area
    QRect visibleRect = m_playlistList->viewport()->rect();

    // Find first and last visible items
    QListWidgetItem* topItem = m_playlistList->itemAt(visibleRect.topLeft());
    QListWidgetItem* bottomItem = m_playlistList->itemAt(visibleRect.bottomLeft());

    if (!topItem || !bottomItem) {
        return;
    }

    int firstVisible = m_playlistList->row(topItem);
    int lastVisible = m_playlistList->row(bottomItem);

    // Load a few items above and below for smooth scrolling
    int bufferSize = 5;
    firstVisible = qMax(0, firstVisible - bufferSize);
    lastVisible = qMin(m_filteredPlaylists.size() - 1, lastVisible + bufferSize);

    // Load playlist covers for visible items
    for (int i = firstVisible; i <= lastVisible; ++i) {
        if (i >= 0 && i < m_filteredPlaylists.size()) {
            QListWidgetItem* item = m_playlistList->item(i);

            // Skip if already loaded/loading
            if (m_loadedItems.contains(item)) {
                continue;
            }

            const auto& playlist = m_filteredPlaylists[i];
            if (!playlist->coverUrl().isEmpty()) {
                m_loadedItems.insert(item);  // Mark as loading
                loadPlaylistCover(playlist->coverUrl(), item);
            }
        }
    }
}

void PlaylistWidget::loadPlaylistCover(const QString& url, QListWidgetItem* item)
{
    QNetworkRequest request(url);
    QNetworkReply* reply = m_imageLoader->get(request);
    m_pendingImages[reply] = item;
}

void PlaylistWidget::onImageDownloaded(QNetworkReply* reply)
{
    if (!reply) {
        emit debugLog("[PlaylistWidget] onImageDownloaded: reply is null");
        return;
    }

    reply->deleteLater();

    if (!m_pendingImages.contains(reply)) {
        emit debugLog("[PlaylistWidget] onImageDownloaded: reply not in pending images");
        return;
    }

    QListWidgetItem* item = m_pendingImages.take(reply);
    QString url = reply->url().toString();

    if (reply->error() == QNetworkReply::NoError) {
        QByteArray imageData = reply->readAll();

        QPixmap pixmap;
        if (pixmap.loadFromData(imageData)) {
            // Scale to fit icon size while maintaining aspect ratio
            QPixmap scaled = pixmap.scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            item->setIcon(QIcon(scaled));
        } else {
            emit debugLog("[PlaylistWidget] Failed to load pixmap from data");
        }
    } else {
        emit debugLog(QString("[PlaylistWidget] Failed to download playlist cover from %1: %2")
                     .arg(url, reply->errorString()));
    }
}
