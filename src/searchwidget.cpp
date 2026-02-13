#include "searchwidget.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollBar>
#include <QNetworkRequest>
#include <QMenu>

SearchWidget::SearchWidget(QWidget *parent)
    : QWidget(parent)
    , m_deezerAPI(nullptr)
    , m_imageLoader(new QNetworkAccessManager(this))
{
    setupUI();
    connect(m_imageLoader, &QNetworkAccessManager::finished,
            this, &SearchWidget::onImageDownloaded);
}

void SearchWidget::setupUI()
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);

    // Title
    QLabel* titleLabel = new QLabel("Search", this);
    QFont titleFont = titleLabel->font();
    titleFont.setPointSize(14);
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);
    mainLayout->addWidget(titleLabel);

    // Search bar
    QHBoxLayout* searchLayout = new QHBoxLayout();
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText("Search for tracks, albums, or playlists...");
    m_searchButton = new QPushButton("Search", this);
    searchLayout->addWidget(m_searchEdit);
    searchLayout->addWidget(m_searchButton);
    mainLayout->addLayout(searchLayout);

    // Results tabs
    m_resultsTabWidget = new QTabWidget(this);

    // Tracks tab
    m_tracksList = new QListWidget(this);
    m_tracksList->setStyleSheet("QListWidget::item { padding: 5px; }");
    m_tracksList->setIconSize(QSize(48, 48));
    m_tracksList->setContextMenuPolicy(Qt::CustomContextMenu);
    m_resultsTabWidget->addTab(m_tracksList, "Tracks");

    // Albums tab
    m_albumsList = new QListWidget(this);
    m_albumsList->setStyleSheet("QListWidget::item { padding: 5px; }");
    m_albumsList->setIconSize(QSize(64, 64));
    m_resultsTabWidget->addTab(m_albumsList, "Albums");

    // Playlists tab
    m_playlistsList = new QListWidget(this);
    m_playlistsList->setStyleSheet("QListWidget::item { padding: 5px; }");
    m_playlistsList->setIconSize(QSize(64, 64));
    m_resultsTabWidget->addTab(m_playlistsList, "Playlists");

    mainLayout->addWidget(m_resultsTabWidget);

    // Connect signals
    connect(m_searchButton, &QPushButton::clicked, this, &SearchWidget::onSearchTriggered);
    connect(m_searchEdit, &QLineEdit::returnPressed, this, &SearchWidget::onSearchTriggered);
    connect(m_resultsTabWidget, &QTabWidget::currentChanged, this, &SearchWidget::onTabChanged);
    connect(m_tracksList, &QListWidget::itemDoubleClicked, this, &SearchWidget::onTrackItemDoubleClicked);
    connect(m_albumsList, &QListWidget::itemDoubleClicked, this, &SearchWidget::onAlbumItemDoubleClicked);
    connect(m_playlistsList, &QListWidget::itemDoubleClicked, this, &SearchWidget::onPlaylistItemDoubleClicked);
    connect(m_tracksList, &QListWidget::customContextMenuRequested, this, &SearchWidget::onTracksContextMenu);

    // Connect scroll events for lazy loading
    connect(m_tracksList->verticalScrollBar(), &QScrollBar::valueChanged,
            this, &SearchWidget::onTrackScrolled);
    connect(m_albumsList->verticalScrollBar(), &QScrollBar::valueChanged,
            this, &SearchWidget::onAlbumScrolled);
    connect(m_playlistsList->verticalScrollBar(), &QScrollBar::valueChanged,
            this, &SearchWidget::onPlaylistScrolled);
}

void SearchWidget::setDeezerAPI(DeezerAPI* api)
{
    m_deezerAPI = api;

    if (m_deezerAPI) {
        // Connect to search-specific signals with context to avoid syncing with other tabs
        connect(m_deezerAPI, &DeezerAPI::searchTracksFound, this, &SearchWidget::onTracksFound);
        connect(m_deezerAPI, &DeezerAPI::searchAlbumsFound, this, &SearchWidget::onAlbumsFound);
        connect(m_deezerAPI, &DeezerAPI::playlistsFound, this, &SearchWidget::onPlaylistsFound);
    }
}

void SearchWidget::onSearchTriggered()
{
    QString query = m_searchEdit->text().trimmed();
    if (query.isEmpty() || !m_deezerAPI) {
        return;
    }

    emit debugLog(QString("[SearchWidget] Searching for: %1").arg(query));

    // Clear previous results
    m_tracksList->clear();
    m_albumsList->clear();
    m_playlistsList->clear();
    m_tracks.clear();
    m_albums.clear();
    m_playlists.clear();
    m_loadedTrackItems.clear();
    m_loadedAlbumItems.clear();
    m_loadedPlaylistItems.clear();
    m_pendingImages.clear();

    // Only search for the currently focused tab to reduce unnecessary API calls
    int currentTab = m_resultsTabWidget->currentIndex();

    switch (currentTab) {
    case 0:  // Tracks tab
        emit debugLog("[SearchWidget] Searching for tracks");
        m_deezerAPI->searchTracksWithContext(query, 50, this);
        break;
    case 1:  // Albums tab
        emit debugLog("[SearchWidget] Searching for albums");
        m_deezerAPI->searchAlbumsWithContext(query, 50, this);
        break;
    case 2:  // Playlists tab
        emit debugLog("[SearchWidget] Searching for playlists");
        // TODO: Implement playlist search when available
        break;
    default:
        emit debugLog("[SearchWidget] Unknown tab index");
        break;
    }
}

void SearchWidget::onTabChanged(int index)
{
    // If there's a search query, automatically search for the newly selected tab
    QString query = m_searchEdit->text().trimmed();
    if (!query.isEmpty() && m_deezerAPI) {
        emit debugLog(QString("[SearchWidget] Tab changed to %1, re-searching").arg(index));
        onSearchTriggered();
    }
}

void SearchWidget::onTracksFound(QList<std::shared_ptr<Track>> tracks, void* sender)
{
    // Only process results if they were requested by this widget
    if (sender && sender != this) {
        emit debugLog(QString("[SearchWidget] Ignoring tracks from different sender"));
        return;
    }

    m_tracks = tracks;
    emit debugLog(QString("[SearchWidget] Found %1 tracks").arg(tracks.size()));

    for (const auto& track : tracks) {
        QString text = QString("%1\n%2 - %3 [%4]")
                          .arg(track->title())
                          .arg(track->artist())
                          .arg(track->album())
                          .arg(track->durationString());

        QListWidgetItem* item = new QListWidgetItem(text, m_tracksList);
        item->setSizeHint(QSize(item->sizeHint().width(), 58));

        // Set placeholder icon
        QPixmap placeholder(48, 48);
        placeholder.fill(Qt::lightGray);
        item->setIcon(QIcon(placeholder));

        m_tracksList->addItem(item);
    }

    loadVisibleTrackArt();
}

void SearchWidget::onAlbumsFound(QList<std::shared_ptr<Album>> albums, void* sender)
{
    // Only process results if they were requested by this widget
    if (sender && sender != this) {
        emit debugLog(QString("[SearchWidget] Ignoring albums from different sender"));
        return;
    }

    m_albums = albums;
    emit debugLog(QString("[SearchWidget] Found %1 albums").arg(albums.size()));

    for (const auto& album : albums) {
        QString text = QString("%1\n%2")
                          .arg(album->title())
                          .arg(album->artist());

        QListWidgetItem* item = new QListWidgetItem(text, m_albumsList);
        item->setSizeHint(QSize(item->sizeHint().width(), 74));

        // Set placeholder icon
        QPixmap placeholder(64, 64);
        placeholder.fill(Qt::lightGray);
        item->setIcon(QIcon(placeholder));

        m_albumsList->addItem(item);
    }

    loadVisibleAlbumArt();
}

void SearchWidget::onPlaylistsFound(QList<std::shared_ptr<Playlist>> playlists)
{
    m_playlists = playlists;
    emit debugLog(QString("[SearchWidget] Found %1 playlists").arg(playlists.size()));

    for (const auto& playlist : playlists) {
        QString text = QString("%1\n%2 tracks")
                          .arg(playlist->title())
                          .arg(playlist->trackCount());

        QListWidgetItem* item = new QListWidgetItem(text, m_playlistsList);
        item->setSizeHint(QSize(item->sizeHint().width(), 74));

        // Set placeholder icon
        QPixmap placeholder(64, 64);
        placeholder.fill(Qt::lightGray);
        item->setIcon(QIcon(placeholder));

        m_playlistsList->addItem(item);
    }

    loadVisiblePlaylistCovers();
}

void SearchWidget::onTrackItemDoubleClicked(QListWidgetItem* item)
{
    int index = m_tracksList->row(item);
    emit debugLog(QString("[SearchWidget] Track item double-clicked at index %1 (total tracks: %2)")
                  .arg(index).arg(m_tracks.size()));

    if (index >= 0 && index < m_tracks.size()) {
        auto track = m_tracks[index];
        emit debugLog(QString("[SearchWidget] Emitting track: %1 (ID: %2, Token: %3)")
                      .arg(track->title(), track->id(),
                           track->trackToken().isEmpty() ? "EMPTY" : "present"));
        emit trackDoubleClicked(track);
    } else {
        emit debugLog(QString("[SearchWidget] Index out of bounds!"));
    }
}

void SearchWidget::onAlbumItemDoubleClicked(QListWidgetItem* item)
{
    int index = m_albumsList->row(item);
    if (index >= 0 && index < m_albums.size()) {
        emit albumDoubleClicked(m_albums[index]);
    }
}

void SearchWidget::onPlaylistItemDoubleClicked(QListWidgetItem* item)
{
    int index = m_playlistsList->row(item);
    if (index >= 0 && index < m_playlists.size()) {
        emit playlistDoubleClicked(m_playlists[index]);
    }
}

void SearchWidget::onTrackScrolled()
{
    loadVisibleTrackArt();
}

void SearchWidget::onAlbumScrolled()
{
    loadVisibleAlbumArt();
}

void SearchWidget::onPlaylistScrolled()
{
    loadVisiblePlaylistCovers();
}

void SearchWidget::loadVisibleTrackArt()
{
    QRect visibleRect = m_tracksList->viewport()->rect();
    QListWidgetItem* topItem = m_tracksList->itemAt(visibleRect.topLeft());
    QListWidgetItem* bottomItem = m_tracksList->itemAt(visibleRect.bottomLeft());

    if (!topItem || !bottomItem) return;

    int firstVisible = m_tracksList->row(topItem);
    int lastVisible = m_tracksList->row(bottomItem);
    int bufferSize = 5;
    firstVisible = qMax(0, firstVisible - bufferSize);
    lastVisible = qMin(m_tracks.size() - 1, lastVisible + bufferSize);

    for (int i = firstVisible; i <= lastVisible; ++i) {
        if (i >= 0 && i < m_tracks.size()) {
            QListWidgetItem* item = m_tracksList->item(i);
            if (m_loadedTrackItems.contains(item)) continue;

            const auto& track = m_tracks[i];
            if (!track->albumArt().isEmpty()) {
                m_loadedTrackItems.insert(item);
                loadImage(track->albumArt(), item);
            }
        }
    }
}

void SearchWidget::loadVisibleAlbumArt()
{
    QRect visibleRect = m_albumsList->viewport()->rect();
    QListWidgetItem* topItem = m_albumsList->itemAt(visibleRect.topLeft());
    QListWidgetItem* bottomItem = m_albumsList->itemAt(visibleRect.bottomLeft());

    if (!topItem || !bottomItem) return;

    int firstVisible = m_albumsList->row(topItem);
    int lastVisible = m_albumsList->row(bottomItem);
    int bufferSize = 5;
    firstVisible = qMax(0, firstVisible - bufferSize);
    lastVisible = qMin(m_albums.size() - 1, lastVisible + bufferSize);

    for (int i = firstVisible; i <= lastVisible; ++i) {
        if (i >= 0 && i < m_albums.size()) {
            QListWidgetItem* item = m_albumsList->item(i);
            if (m_loadedAlbumItems.contains(item)) continue;

            const auto& album = m_albums[i];
            if (!album->coverUrl().isEmpty()) {
                m_loadedAlbumItems.insert(item);
                loadImage(album->coverUrl(), item);
            }
        }
    }
}

void SearchWidget::loadVisiblePlaylistCovers()
{
    QRect visibleRect = m_playlistsList->viewport()->rect();
    QListWidgetItem* topItem = m_playlistsList->itemAt(visibleRect.topLeft());
    QListWidgetItem* bottomItem = m_playlistsList->itemAt(visibleRect.bottomLeft());

    if (!topItem || !bottomItem) return;

    int firstVisible = m_playlistsList->row(topItem);
    int lastVisible = m_playlistsList->row(bottomItem);
    int bufferSize = 5;
    firstVisible = qMax(0, firstVisible - bufferSize);
    lastVisible = qMin(m_playlists.size() - 1, lastVisible + bufferSize);

    for (int i = firstVisible; i <= lastVisible; ++i) {
        if (i >= 0 && i < m_playlists.size()) {
            QListWidgetItem* item = m_playlistsList->item(i);
            if (m_loadedPlaylistItems.contains(item)) continue;

            const auto& playlist = m_playlists[i];
            if (!playlist->coverUrl().isEmpty()) {
                m_loadedPlaylistItems.insert(item);
                loadImage(playlist->coverUrl(), item);
            }
        }
    }
}

void SearchWidget::loadImage(const QString& url, QListWidgetItem* item)
{
    QNetworkRequest request(url);
    QNetworkReply* reply = m_imageLoader->get(request);
    m_pendingImages[reply] = item;
}

void SearchWidget::onImageDownloaded(QNetworkReply* reply)
{
    if (!reply) return;
    reply->deleteLater();

    if (!m_pendingImages.contains(reply)) return;

    QListWidgetItem* item = m_pendingImages.take(reply);

    if (reply->error() == QNetworkReply::NoError) {
        QByteArray imageData = reply->readAll();
        QPixmap pixmap;
        if (pixmap.loadFromData(imageData)) {
            // Determine size based on which list owns this item
            int targetSize = 48;  // Default for tracks
            if (m_albumsList->indexFromItem(item).isValid() ||
                m_playlistsList->indexFromItem(item).isValid()) {
                targetSize = 64;
            }
            QPixmap scaled = pixmap.scaled(targetSize, targetSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            item->setIcon(QIcon(scaled));
        }
    }
}

void SearchWidget::onTracksContextMenu(const QPoint& pos)
{
    QListWidgetItem* item = m_tracksList->itemAt(pos);
    if (!item)
        return;

    // Get selected items
    QList<QListWidgetItem*> selectedItems = m_tracksList->selectedItems();
    if (selectedItems.isEmpty())
        return;

    // Gather selected tracks
    QList<std::shared_ptr<Track>> selectedTracks;
    for (QListWidgetItem* selectedItem : selectedItems) {
        int row = m_tracksList->row(selectedItem);
        if (row >= 0 && row < m_tracks.size()) {
            selectedTracks.append(m_tracks[row]);
        }
    }

    if (selectedTracks.isEmpty())
        return;

    // Create context menu
    QMenu contextMenu(this);

    QString trackText = (selectedTracks.size() == 1)
        ? "Track"
        : QString("%1 Tracks").arg(selectedTracks.size());

    QAction* playNextAction = contextMenu.addAction("Play Next");
    QAction* addToQueueAction = contextMenu.addAction(
        QString("Add %1 to Queue").arg(trackText)
    );

    connect(playNextAction, &QAction::triggered, [this, selectedTracks]() {
        emit playNextRequested(selectedTracks);
    });

    connect(addToQueueAction, &QAction::triggered, [this, selectedTracks]() {
        emit addToQueueRequested(selectedTracks);
    });

    contextMenu.exec(m_tracksList->mapToGlobal(pos));
}