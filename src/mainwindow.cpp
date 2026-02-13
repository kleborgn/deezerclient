#include "mainwindow.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QMessageBox>
#include <QInputDialog>
#include <QDesktopServices>
#include <QUrl>
#include <QDialog>
#include <QTabWidget>
#include <QLineEdit>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QDateTime>
#include <QFont>
#include <QCheckBox>
#include "spectrumwindow.h"
#include "lyricswindow.h"
#include "lastfmapi.h"
#include "scrobblecache.h"
#include "lastfmsettingsdialog.h"
#include <QTimer>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_audioEngine(new AudioEngine(this))
    , m_deezerAPI(new DeezerAPI(this))
    , m_imageManager(new QNetworkAccessManager(this))
    , m_discordManager(new DiscordManager())
    , m_discordThread(new QThread(this))
    , m_lastFmAPI(new LastFmAPI(this))
    , m_scrobbleCache(new ScrobbleCache(this))
    , m_scrobbleFetchTimer(new QTimer(this))
{
    qRegisterMetaType<std::shared_ptr<Track>>("std::shared_ptr<Track>");
    m_discordManager->moveToThread(m_discordThread);
    connect(m_discordThread, &QThread::started, [this]() {
        m_discordManager->start("1258131430928547880");
    });
    connect(m_discordThread, &QThread::finished, m_discordManager, &QObject::deleteLater);
    m_discordThread->start();

    setupUI();
    setupMenus();
    createConnections();

    // Configure Last.fm fetch timer for rate limiting (1 second delay between batches)
    m_scrobbleFetchTimer->setSingleShot(true);
    connect(m_scrobbleFetchTimer, &QTimer::timeout, this, &MainWindow::fetchNextBatchOfScrobbles);

    // Initialize audio engine and wire for full-track playback
    if (!m_audioEngine->initialize()) {
        QMessageBox::critical(this, "Error", "Failed to initialize audio engine");
    }
    m_audioEngine->setDeezerAPI(m_deezerAPI);

    // Load saved settings and attempt auto-login
    loadSettings();
    autoLogin();

    setWindowTitle("Deezer Client - Native Desktop");
    resize(1200, 700);
}

MainWindow::~MainWindow()
{
    m_discordThread->quit();
    m_discordThread->wait();
    m_audioEngine->shutdown();
}

void MainWindow::setupUI()
{
    // Create central widget
    QWidget* centralWidget = new QWidget(this);
    QVBoxLayout* mainLayout = new QVBoxLayout(centralWidget);
    
    m_tabWidget = new QTabWidget(this);
    
    // --- TAB 1: LIBRARY ---
    QWidget* libraryTab = new QWidget();
    QVBoxLayout* libraryLayout = new QVBoxLayout(libraryTab);

    m_playlistWidget = new PlaylistWidget(this);
    m_playlistWidget->setDeezerAPI(m_deezerAPI);

    m_trackListWidget = new TrackListWidget(this);
    m_trackListWidget->setDeezerAPI(m_deezerAPI);
    m_trackListWidget->setSearchVisible(false);
    m_trackListWidget->hide(); // Hidden but kept for Now Playing queue display

    m_libraryPlayerControls = new PlayerControls(this);
    m_libraryPlayerControls->setAudioEngine(m_audioEngine);
    m_libraryPlayerControls->hide(); // Hidden - player controls only in Now Playing tab

    libraryLayout->addWidget(m_playlistWidget);
    
    // --- TAB 2: ALBUMS ---
    QWidget* albumsTab = new QWidget();
    QVBoxLayout* albumsLayout = new QVBoxLayout(albumsTab);

    m_albumWidget = new AlbumListWidget(this);
    m_albumWidget->setDeezerAPI(m_deezerAPI);
    albumsLayout->addWidget(m_albumWidget);

    // --- TAB 3: SEARCH ---
    QWidget* searchTab = new QWidget();
    QVBoxLayout* searchLayout = new QVBoxLayout(searchTab);

    m_searchWidget = new SearchWidget(this);
    m_searchWidget->setDeezerAPI(m_deezerAPI);
    searchLayout->addWidget(m_searchWidget);

    // --- TAB 4: NOW PLAYING ---
    QWidget* nowPlayingTab = new QWidget();
    QVBoxLayout* nowPlayingLayout = new QVBoxLayout(nowPlayingTab);
    nowPlayingLayout->setContentsMargins(0, 0, 0, 0);
    nowPlayingLayout->setSpacing(0);

    // Large Album Art (fills left side, no border)
    m_largeAlbumArtLabel = new AspectRatioLabel(nowPlayingTab);
    m_largeAlbumArtLabel->setAlignment(Qt::AlignCenter);
    m_largeAlbumArtLabel->setText("No Track Playing");
    m_largeAlbumArtLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    // Right side: Header + Queue
    QWidget* rightPanel = new QWidget(nowPlayingTab);
    QVBoxLayout* rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);

    m_queueHeader = new QueueHeaderWidget(rightPanel);
    m_queueHeader->hide();

    m_queueWidget = new TrackListWidget(rightPanel);
    m_queueWidget->setSearchVisible(false);

    rightLayout->addWidget(m_queueHeader);
    rightLayout->addWidget(m_queueWidget, 1);

    // Use QSplitter for resizable left/right split
    QSplitter* splitter = new QSplitter(Qt::Horizontal, nowPlayingTab);
    splitter->addWidget(m_largeAlbumArtLabel);
    splitter->addWidget(rightPanel);
    splitter->setSizes({500, 500});
    splitter->setChildrenCollapsible(false);

    m_nowPlayingPlayerControls = new PlayerControls(nowPlayingTab);
    m_nowPlayingPlayerControls->setAudioEngine(m_audioEngine);

    nowPlayingLayout->addWidget(splitter, 1);
    nowPlayingLayout->addWidget(m_nowPlayingPlayerControls);
    
    // Add tabs
    m_tabWidget->addTab(libraryTab, "Playlists");
    m_tabWidget->addTab(albumsTab, "Albums");
    m_tabWidget->addTab(searchTab, "Search");
    m_tabWidget->addTab(nowPlayingTab, "Now Playing");
    
    // Add to main layout
    mainLayout->addWidget(m_tabWidget);
    
    setCentralWidget(centralWidget);
    
    // Status bar
    statusBar()->showMessage("Ready");
}

void MainWindow::setupMenus()
{
    // File menu
    m_fileMenu = menuBar()->addMenu("&File");
    
    m_loginAction = m_fileMenu->addAction("&Login to Deezer");
    m_logoutAction = m_fileMenu->addAction("Log&out");
    m_logoutAction->setEnabled(false);
    
    m_fileMenu->addSeparator();
    
    QAction* quitAction = m_fileMenu->addAction("&Quit");
    quitAction->setShortcut(QKeySequence::Quit);
    
    // Settings menu
    m_settingsMenu = menuBar()->addMenu("&Settings");
    
    m_gaplessAction = m_settingsMenu->addAction("&Gapless Playback");
    m_gaplessAction->setCheckable(true);
    m_gaplessAction->setChecked(true);
    
    m_discordRpcAction = m_settingsMenu->addAction("&Discord Presence");
    m_discordRpcAction->setCheckable(true);
    m_discordRpcAction->setChecked(true);

    m_spectrumAction = m_settingsMenu->addAction("&Spectrum Visualizer");
    m_spectrumAction->setCheckable(true);
    m_spectrumAction->setChecked(false);

    m_lyricsAction = m_settingsMenu->addAction("L&yrics");
    m_lyricsAction->setCheckable(true);
    m_lyricsAction->setChecked(false);

    QAction* projectMAction = m_settingsMenu->addAction("&projectM Visualizer");
    connect(projectMAction, &QAction::triggered, this, [this]() {
        if (!m_projectMWindow) {
            m_projectMWindow = new ProjectMWindow(nullptr);  // No parent to avoid main window repaint
            m_projectMWindow->setAudioEngine(m_audioEngine);
            connect(m_projectMWindow, &ProjectMWindow::debugLog, this, &MainWindow::onDebugLog);
        }
        m_projectMWindow->show();
        m_projectMWindow->raise();
    });

    m_settingsMenu->addSeparator();
    m_lastFmSettingsAction = m_settingsMenu->addAction("&Last.fm Settings...");
    connect(m_lastFmSettingsAction, &QAction::triggered, this, &MainWindow::onLastFmSettingsClicked);

    // Help menu
    m_helpMenu = menuBar()->addMenu("&Help");
    
    QAction* debugLogAction = m_helpMenu->addAction("View &debug log");
    QAction* aboutAction = m_helpMenu->addAction("&About");
    
    // Connect menu actions
    connect(m_loginAction, &QAction::triggered, this, &MainWindow::onLoginClicked);
    connect(m_logoutAction, &QAction::triggered, this, &MainWindow::onLogoutClicked);
    connect(quitAction, &QAction::triggered, this, &MainWindow::onQuitClicked);
    connect(debugLogAction, &QAction::triggered, this, &MainWindow::onViewDebugLog);
    connect(aboutAction, &QAction::triggered, this, &MainWindow::onAboutClicked);
    connect(m_gaplessAction, &QAction::toggled, this, &MainWindow::onToggleGaplessClicked);
    connect(m_discordRpcAction, &QAction::toggled, this, &MainWindow::onToggleDiscordRpcClicked);
    connect(m_spectrumAction, &QAction::toggled, this, &MainWindow::onToggleSpectrumClicked);
    connect(m_lyricsAction, &QAction::toggled, this, &MainWindow::onToggleLyricsClicked);
}

void MainWindow::createConnections()
{
    // Deezer API
    connect(m_deezerAPI, &DeezerAPI::authenticated, this, &MainWindow::onAuthenticated);
    connect(m_deezerAPI, &DeezerAPI::authenticationFailed, this, &MainWindow::onAuthenticationFailed);
    connect(m_deezerAPI, &DeezerAPI::error, this, &MainWindow::onError);
    connect(m_deezerAPI, &DeezerAPI::debugLog, this, &MainWindow::onDebugLog);
    connect(this, &MainWindow::debugLog, this, &MainWindow::onDebugLog);

    // Playlist
    connect(m_playlistWidget, &PlaylistWidget::playlistDoubleClicked, this, &MainWindow::onPlaylistDoubleClicked);
    connect(m_deezerAPI, &DeezerAPI::playlistReceived, this, &MainWindow::onPlaylistReceived);
    connect(m_playlistWidget, &PlaylistWidget::debugLog, this, &MainWindow::onDebugLog);

    // Album
    connect(m_albumWidget, &AlbumListWidget::albumDoubleClicked, this, &MainWindow::onAlbumDoubleClicked);
    connect(m_deezerAPI, &DeezerAPI::albumReceived, this, &MainWindow::onAlbumReceived);
    connect(m_albumWidget, &AlbumListWidget::debugLog, this, &MainWindow::onDebugLog);

    // Track (for fetching full details on demand)
    connect(m_deezerAPI, &DeezerAPI::trackReceived, this, &MainWindow::onTrackReceived);

    // Lyrics
    connect(m_deezerAPI, &DeezerAPI::lyricsReceived, this, &MainWindow::onLyricsReceived);

    // Search
    connect(m_searchWidget, &SearchWidget::trackDoubleClicked, this, &MainWindow::onTrackDoubleClicked);
    connect(m_searchWidget, &SearchWidget::albumDoubleClicked, this, &MainWindow::onAlbumDoubleClicked);
    connect(m_searchWidget, &SearchWidget::playlistDoubleClicked, this, &MainWindow::onPlaylistDoubleClicked);
    connect(m_searchWidget, &SearchWidget::debugLog, this, &MainWindow::onDebugLog);

    // Search - Add to Queue
    connect(m_searchWidget, &SearchWidget::addToQueueRequested,
            this, [this](QList<std::shared_ptr<Track>> tracks) {
        m_audioEngine->addToQueue(tracks);  // Append to end
    });

    connect(m_searchWidget, &SearchWidget::playNextRequested,
            this, [this](QList<std::shared_ptr<Track>> tracks) {
        int insertPos = m_audioEngine->currentIndex() + 1;
        m_audioEngine->addToQueue(tracks, insertPos);
    });

    // Player controls (Now Playing only - Library player controls removed)
    connect(m_nowPlayingPlayerControls, &PlayerControls::playClicked, this, &MainWindow::onPlayClicked);
    connect(m_nowPlayingPlayerControls, &PlayerControls::pauseClicked, this, &MainWindow::onPauseClicked);
    connect(m_nowPlayingPlayerControls, &PlayerControls::stopClicked, this, &MainWindow::onStopClicked);
    connect(m_nowPlayingPlayerControls, &PlayerControls::nextClicked, this, &MainWindow::onNextClicked);
    connect(m_nowPlayingPlayerControls, &PlayerControls::previousClicked, this, &MainWindow::onPreviousClicked);
    connect(m_nowPlayingPlayerControls, &PlayerControls::seekRequested, this, &MainWindow::onSeekRequested);
    connect(m_nowPlayingPlayerControls, &PlayerControls::volumeChanged, this, &MainWindow::onVolumeChanged);
    
    // Audio engine
    connect(m_deezerAPI, &DeezerAPI::streamUrlReceived, m_audioEngine, &AudioEngine::onStreamUrlReceived);
    connect(m_audioEngine, &AudioEngine::error, this, &MainWindow::onError);
    connect(m_audioEngine, &AudioEngine::debugLog, this, &MainWindow::onDebugLog);

    // Stream info to queue header
    connect(m_audioEngine, &AudioEngine::streamInfoChanged, m_queueHeader, &QueueHeaderWidget::setStreamInfo);
    
    // Sync queue widget in Now Playing tab
    connect(m_audioEngine, &AudioEngine::trackChanged, [this](std::shared_ptr<Track> track) {
        if (track) m_queueWidget->setCurrentTrackId(track->id());
    });
    connect(m_audioEngine, &AudioEngine::queueChanged, [this]() {
        m_queueWidget->setTracks(m_audioEngine->queue());
        if (m_audioEngine->currentTrack())
            m_queueWidget->setCurrentTrackId(m_audioEngine->currentTrack()->id());
    });
    
    // Track from queue table in Now Playing
    connect(m_queueWidget, &TrackListWidget::trackDoubleClicked, this, &MainWindow::onTrackDoubleClicked);

    // Queue management - set queue widget to queue mode
    m_queueWidget->setMode(TrackListWidget::QueueMode);

    // Queue management signals
    connect(m_queueWidget, &TrackListWidget::removeRequested,
            m_audioEngine, QOverload<int>::of(&AudioEngine::removeFromQueue));

    connect(m_queueWidget, &TrackListWidget::removeMultipleRequested,
            m_audioEngine, QOverload<const QList<int>&>::of(&AudioEngine::removeFromQueue));

    connect(m_queueWidget, &TrackListWidget::moveRequested,
            m_audioEngine, &AudioEngine::moveInQueue);

    // Favorite toggle
    connect(m_queueWidget, &TrackListWidget::favoriteToggled,
            this, [this](std::shared_ptr<Track> track, bool isFavorite) {
        QString ctxType = m_audioEngine->contextType();
        QString ctxId = m_audioEngine->contextId();
        if (isFavorite) {
            m_deezerAPI->addFavoriteTrack(track->id(), ctxType, ctxId);
        } else {
            m_deezerAPI->removeFavoriteTrack(track->id(), ctxType, ctxId);
        }
    });

    // Sync "Now Playing" visuals
    connect(m_audioEngine, &AudioEngine::trackChanged, this, [this](std::shared_ptr<Track> track) {
        if (track) {
            QString artUrl = track->albumArt();
            if (!artUrl.isEmpty()) {
                QNetworkReply* reply = m_imageManager->get(QNetworkRequest(QUrl(artUrl)));
                connect(reply, &QNetworkReply::finished, [this, reply]() {
                    if (reply->error() == QNetworkReply::NoError) {
                        QByteArray data = reply->readAll();
                        QPixmap pixmap;
                        if (pixmap.loadFromData(data)) {
                            m_largeAlbumArtLabel->setPixmap(pixmap);
                        }
                    }
                    reply->deleteLater();
                });
            } else {
                m_largeAlbumArtLabel->setPixmap(QPixmap());
                m_largeAlbumArtLabel->setText("No Art");
            }
        } else {
            m_largeAlbumArtLabel->setPixmap(QPixmap());
            m_largeAlbumArtLabel->setText("No Track Playing");
        }
    });

    connect(m_audioEngine, &AudioEngine::queueChanged, this, [this]() {
        m_queueWidget->setTracks(m_audioEngine->queue());
    });
    
    connect(m_audioEngine, &AudioEngine::trackChanged, m_discordManager, [this](std::shared_ptr<Track> track) {
        // Capture data on UI thread
        bool isPlaying = (m_audioEngine->state() == AudioEngine::Playing);
        int pos = m_audioEngine->positionSeconds();

        QMetaObject::invokeMethod(m_discordManager, "updatePresence",
            Qt::QueuedConnection,
            Q_ARG(std::shared_ptr<Track>, track),
            Q_ARG(bool, isPlaying),
            Q_ARG(int, pos));
    });

    // Fetch lyrics when track changes
    connect(m_audioEngine, &AudioEngine::trackChanged, this, [this](std::shared_ptr<Track> track) {
        if (track && m_lyricsWindow) {
            // Only fetch if lyrics window is open and track doesn't have lyrics cached
            if (track->lyrics().isEmpty() && track->syncedLyrics().isEmpty()) {
                m_deezerAPI->getLyrics(track->id());
            }
        }
    });

    connect(m_audioEngine, &AudioEngine::stateChanged, m_discordManager, [this]() {
        // Capture data on UI thread
        auto track = m_audioEngine->currentTrack();
        bool isPlaying = (m_audioEngine->state() == AudioEngine::Playing);
        int pos = m_audioEngine->positionSeconds();
        
        QMetaObject::invokeMethod(m_discordManager, "updatePresence", 
            Qt::QueuedConnection, 
            Q_ARG(std::shared_ptr<Track>, track),
            Q_ARG(bool, isPlaying),
            Q_ARG(int, pos));
    });

    connect(m_discordManager, &DiscordManager::debugLog, this, &MainWindow::onDebugLog, Qt::QueuedConnection);

    // Last.fm
    connect(m_lastFmAPI, &LastFmAPI::trackInfoReceived, this, &MainWindow::onLastFmTrackInfoReceived);
    connect(m_lastFmAPI, &LastFmAPI::albumInfoReceived, this, &MainWindow::onLastFmAlbumInfoReceived);
    connect(m_lastFmAPI, &LastFmAPI::authenticated, this, &MainWindow::onLastFmAuthenticated);
    connect(m_lastFmAPI, &LastFmAPI::error, this, &MainWindow::onError);

    // Fetch scrobble data when queue changes
    connect(m_audioEngine, &AudioEngine::queueChanged, this, &MainWindow::fetchScrobbleDataForQueue);
}

void MainWindow::onLoginClicked()
{
    // Show dialog with two login options
    QDialog dialog(this);
    dialog.setWindowTitle("Login to Deezer");

    QVBoxLayout* layout = new QVBoxLayout(&dialog);

    QLabel* infoLabel = new QLabel(
        "Choose login method:\n\n"
        "Method 1: Email & Password (requires API key)\n"
        "Method 2: ARL cookie (recommended - no API key needed)", &dialog);
    layout->addWidget(infoLabel);

    QTabWidget* tabWidget = new QTabWidget(&dialog);

    // Tab 1: Email/Password
    QWidget* emailTab = new QWidget();
    QFormLayout* emailLayout = new QFormLayout(emailTab);

    QLineEdit* emailEdit = new QLineEdit();
    QLineEdit* passwordEdit = new QLineEdit();
    passwordEdit->setEchoMode(QLineEdit::Password);

    emailLayout->addRow("Email:", emailEdit);
    emailLayout->addRow("Password:", passwordEdit);

    QPushButton* emailLoginBtn = new QPushButton("Login with Email");
    emailLayout->addRow(emailLoginBtn);

    tabWidget->addTab(emailTab, "Email & Password");

    // Tab 2: ARL
    QWidget* arlTab = new QWidget();
    QVBoxLayout* arlLayout = new QVBoxLayout(arlTab);

    QLabel* arlInfo = new QLabel(
        "To get your ARL:\n"
        "1. Open deezer.com in your browser\n"
        "2. Login to your account\n"
        "3. Press F12 to open developer tools\n"
        "4. Go to Application tab (Chrome) or Storage tab (Firefox)\n"
        "5. Click on Cookies → https://www.deezer.com\n"
        "6. Find 'arl' cookie and copy its value\n\n"
        "Paste the ARL below:", arlTab);
    arlLayout->addWidget(arlInfo);

    QLineEdit* arlEdit = new QLineEdit();
    arlLayout->addWidget(arlEdit);

    QPushButton* arlLoginBtn = new QPushButton("Login with ARL");
    arlLayout->addWidget(arlLoginBtn);

    tabWidget->addTab(arlTab, "ARL Cookie");

    layout->addWidget(tabWidget);

    // Add "Remember me" checkbox
    QCheckBox* rememberMeCheckbox = new QCheckBox("Remember me", &dialog);
    rememberMeCheckbox->setChecked(true); // Default to checked for convenience
    layout->addWidget(rememberMeCheckbox);

    QPushButton* cancelBtn = new QPushButton("Cancel");
    layout->addWidget(cancelBtn);

    // Connect buttons
    connect(emailLoginBtn, &QPushButton::clicked, &dialog, [&, this]() {
        QString email = emailEdit->text().trimmed();
        QString password = passwordEdit->text();

        if (email.isEmpty() || password.isEmpty()) {
            QMessageBox::warning(&dialog, "Error", "Please enter both email and password");
            return;
        }

        // Check if API key is set
        if (DeezerAPI::apiKey().isEmpty()) {
            QMessageBox::warning(&dialog, "API Key Required",
                "To login with email, you need to set the Deezer API key first.\n\n"
                "See README.md for instructions on obtaining the API key.\n\n"
                "Alternatively, use the ARL login method.");
            return;
        }

        // Store checkbox state in settings temporarily (will save ARL on success)
        QSettings settings;
        settings.setValue("Authentication/rememberMe", rememberMeCheckbox->isChecked());

        m_deezerAPI->signInWithEmail(email, password);
        dialog.accept();
    });

    connect(arlLoginBtn, &QPushButton::clicked, &dialog, [&, this]() {
        QString arl = arlEdit->text().trimmed();

        if (arl.isEmpty()) {
            QMessageBox::warning(&dialog, "Error", "Please enter your ARL");
            return;
        }

        // Store checkbox state
        QSettings settings;
        settings.setValue("Authentication/rememberMe", rememberMeCheckbox->isChecked());

        m_deezerAPI->signInWithArl(arl);
        dialog.accept();
    });

    connect(cancelBtn, &QPushButton::clicked, &dialog, &QDialog::reject);

    dialog.exec();
}

void MainWindow::onLogoutClicked()
{
    m_deezerAPI->logout();
    updateLoginState();

    // Clear saved authentication
    QSettings settings;
    settings.remove("Authentication/arl");
    settings.remove("Authentication/rememberMe");

    statusBar()->showMessage("Logged out");
    emit debugLog("[Settings] Cleared saved credentials");
}

void MainWindow::onQuitClicked()
{
    close();
}

void MainWindow::onAboutClicked()
{
    QMessageBox::about(this, "About Deezer Client",
                      "Deezer Desktop Client v1.0\n\n"
                      "A native Windows desktop client for Deezer with gapless playback.\n\n"
                      "Built with Qt 6 and BASS audio library.\n\n"
                      "Features:\n"
                      "• Gapless playback\n"
                      "• Native performance\n"
                      "• Clean interface\n"
                      "• Low resource usage");
}

void MainWindow::onToggleGaplessClicked(bool checked)
{
    m_audioEngine->setGaplessEnabled(checked);
    statusBar()->showMessage(QString("Gapless Playback: %1").arg(checked ? "Enabled" : "Disabled"));

    // Save preference immediately
    QSettings settings;
    settings.setValue("Preferences/gaplessPlayback", checked);
}

void MainWindow::onToggleDiscordRpcClicked(bool checked)
{
    QMetaObject::invokeMethod(m_discordManager, "setEnabled",
        Qt::QueuedConnection, Q_ARG(bool, checked));

    statusBar()->showMessage(QString("Discord Presence: %1").arg(checked ? "Enabled" : "Disabled"));

    if (checked) {
        // Force an immediate update
        auto track = m_audioEngine->currentTrack();
        bool isPlaying = (m_audioEngine->state() == AudioEngine::Playing);
        int pos = m_audioEngine->positionSeconds();

        QMetaObject::invokeMethod(m_discordManager, "updatePresence",
            Qt::QueuedConnection,
            Q_ARG(std::shared_ptr<Track>, track),
            Q_ARG(bool, isPlaying),
            Q_ARG(int, pos));
    }

    // Save preference immediately
    QSettings settings;
    settings.setValue("Preferences/discordRPC", checked);
}

void MainWindow::onToggleSpectrumClicked(bool checked)
{
    if (checked) {
        if (!m_spectrumWindow) {
            m_spectrumWindow = new SpectrumWindow(nullptr);  // No parent to avoid main window repaint
            m_spectrumWindow->setAudioEngine(m_audioEngine);

            // When spectrum window closes, uncheck the menu action
            connect(m_spectrumWindow, &SpectrumWindow::closed, this, [this]() {
                m_spectrumAction->setChecked(false);
            });

            // Restore geometry if saved
            QSettings settings;
            QByteArray geometry = settings.value("SpectrumWindow/geometry").toByteArray();
            if (!geometry.isEmpty()) {
                m_spectrumWindow->restoreGeometry(geometry);
            }
        }
        m_spectrumWindow->show();
        m_audioEngine->setSpectrumEnabled(true);
        statusBar()->showMessage("Spectrum Visualizer: Enabled");
    } else {
        if (m_spectrumWindow) {
            m_spectrumWindow->hide();
        }
        m_audioEngine->setSpectrumEnabled(false);
        statusBar()->showMessage("Spectrum Visualizer: Disabled");
    }

    // Save preference immediately
    QSettings settings;
    settings.setValue("Preferences/spectrum", checked);
}

void MainWindow::onToggleLyricsClicked(bool checked)
{
    if (checked) {
        if (!m_lyricsWindow) {
            m_lyricsWindow = new LyricsWindow(nullptr);  // No parent to avoid main window repaint
            m_lyricsWindow->setAudioEngine(m_audioEngine);

            // When lyrics window closes, uncheck the menu action
            connect(m_lyricsWindow, &LyricsWindow::closed, this, [this]() {
                m_lyricsAction->setChecked(false);
            });

            // Forward debug logs
            connect(m_lyricsWindow, &LyricsWindow::debugLog, this, &MainWindow::onDebugLog);

            // Restore geometry if saved
            QSettings settings;
            QByteArray geometry = settings.value("LyricsWindow/geometry").toByteArray();
            if (!geometry.isEmpty()) {
                m_lyricsWindow->restoreGeometry(geometry);
            }
        }
        m_lyricsWindow->show();
        statusBar()->showMessage("Lyrics: Enabled");

        // Fetch lyrics for current track if available
        auto currentTrack = m_audioEngine->currentTrack();
        if (currentTrack) {
            if (currentTrack->lyrics().isEmpty() && currentTrack->syncedLyrics().isEmpty()) {
                m_deezerAPI->getLyrics(currentTrack->id());
            } else {
                m_lyricsWindow->onTrackChanged(currentTrack);
            }
        }
    } else {
        if (m_lyricsWindow) {
            m_lyricsWindow->hide();
        }
        statusBar()->showMessage("Lyrics: Disabled");
    }

    // Save preference immediately
    QSettings settings;
    settings.setValue("Preferences/lyrics", checked);
}

void MainWindow::onLyricsReceived(const QString& trackId, const QString& lyrics, const QJsonArray& syncedLyrics)
{
    // Update the track with lyrics
    auto currentTrack = m_audioEngine->currentTrack();
    if (currentTrack && currentTrack->id() == trackId) {
        currentTrack->setLyrics(lyrics);
        currentTrack->setSyncedLyrics(syncedLyrics);

        // If lyrics window is open, update it
        if (m_lyricsWindow && m_lyricsWindow->isVisible()) {
            m_lyricsWindow->updateLyrics(trackId, lyrics, syncedLyrics);
        }
    }

    // Also check queue for this track
    for (auto& track : m_currentQueue) {
        if (track && track->id() == trackId) {
            track->setLyrics(lyrics);
            track->setSyncedLyrics(syncedLyrics);
        }
    }
}

void MainWindow::onAuthenticated(const QString& username)
{
    updateLoginState();
    statusBar()->showMessage("Successfully logged in as: " + username);

    // Save ARL if "Remember me" was checked
    QSettings settings;
    bool rememberMe = settings.value("Authentication/rememberMe", false).toBool();

    if (rememberMe) {
        QString arl = m_deezerAPI->arl();
        if (!arl.isEmpty()) {
            settings.setValue("Authentication/arl", arl);
            emit debugLog("[Settings] Saved ARL for auto-login");
        }
    }

    // Clear the temporary remember-me flag
    settings.remove("Authentication/rememberMe");

    // Load user data
    m_deezerAPI->getUserPlaylists();
    m_deezerAPI->getUserAlbums();
    m_deezerAPI->fetchFavoriteTrackIds();
}

void MainWindow::onAuthenticationFailed(const QString& error)
{
    QMessageBox::warning(this, "Authentication Failed", 
        "Login failed: " + error + "\n\n"
        "If using email/password, make sure you have set the API key.\n"
        "If using ARL, make sure the cookie is valid and not expired.");
    statusBar()->showMessage("Authentication failed");
}

void MainWindow::onTrackDoubleClicked(std::shared_ptr<Track> track)
{
    if (!track) return;

    emit debugLog(QString("[MainWindow] Track double-clicked: %1 (ID: %2, Token: %3)")
                  .arg(track->title(), track->id(),
                       track->trackToken().isEmpty() ? "EMPTY" : "present"));

    // Check if track has the required token for playback
    if (track->trackToken().isEmpty()) {
        // Track is missing TRACK_TOKEN (e.g., from search results)
        // Fetch full track details first
        emit debugLog(QString("[MainWindow] Fetching track details for ID: %1").arg(track->id()));
        statusBar()->showMessage("Loading track: " + track->title() + "...");
        m_pendingTrackPlayback = true;
        m_deezerAPI->getTrack(track->id());
        return;
    }

    TrackListWidget* senderWidget = qobject_cast<TrackListWidget*>(sender());

    if (senderWidget) {
        const auto& allTracks = senderWidget->tracks();
        int idx = allTracks.indexOf(track);

        if (idx >= 0) {
            m_audioEngine->setQueue(allTracks);
            m_audioEngine->playAtIndex(idx);
        } else {
            // Fallback for tracks not in the current list
            m_audioEngine->loadTrack(track);
        }
    } else {
        // Sender is not a TrackListWidget (e.g., from search widget)
        // Play single track
        m_audioEngine->loadTrack(track);
    }
    m_tabWidget->setCurrentIndex(3); // Switch to "Now Playing" tab
    statusBar()->showMessage("Playing: " + track->title() + " - " + track->artist());
}

void MainWindow::onTracksSelected(QList<std::shared_ptr<Track>> tracks)
{
    m_currentQueue = tracks;
    m_audioEngine->setQueue(tracks);
}

void MainWindow::onPlaylistDoubleClicked(std::shared_ptr<Playlist> playlist)
{
    // In a real implementation, you'd fetch the playlist tracks first
    // For now, just show a message
    statusBar()->showMessage("Loading playlist: " + playlist->title());
    m_deezerAPI->getPlaylist(playlist->id());
}

void MainWindow::onPlaylistReceived(std::shared_ptr<Playlist> playlist)
{
    if (playlist) {
        m_playlistWidget->setCurrentPlaylist(playlist);

        // Fallback for duration/track count
        if (playlist->trackCount() == 0) {
            playlist->setTrackCount(playlist->tracks().size());
        }
        if (playlist->totalDuration() == 0) {
            int total = 0;
            for (const auto& t : playlist->tracks()) total += t->duration();
            playlist->setTotalDuration(total);
        }

        // Load playlist into queue and start playing
        const auto& tracks = playlist->tracks();
        if (!tracks.isEmpty()) {
            m_audioEngine->setQueue(tracks, "profile_playlists", playlist->id());
            m_audioEngine->playAtIndex(0);
            m_queueHeader->setPlaylist(playlist);
            m_tabWidget->setCurrentIndex(3); // Switch to "Now Playing" tab

            // Clear album reference since this is a playlist
            m_currentAlbumForScrobble.reset();

            statusBar()->showMessage(QString("Playing playlist: %1 (%2 tracks)").arg(playlist->title()).arg(tracks.size()));
        } else {
            statusBar()->showMessage(QString("Playlist is empty: %1").arg(playlist->title()));
        }
    }
}

void MainWindow::onAlbumDoubleClicked(std::shared_ptr<Album> album)
{
    if (album) {
        emit debugLog(QString("Album double-clicked: %1 (ID: %2)").arg(album->title(), album->id()));
        m_deezerAPI->getAlbum(album->id());
    }
}

void MainWindow::onAlbumReceived(std::shared_ptr<Album> album, QList<std::shared_ptr<Track>> tracks)
{
    if (album && !tracks.isEmpty()) {
        // Fallback: if API returned 0 for duration/track count in the top-level object,
        // calculate it from the actual tracks list we received.
        if (album->trackCount() == 0) {
            album->setTrackCount(tracks.size());
        }
        if (album->totalDuration() == 0) {
            int total = 0;
            for (const auto& t : tracks) total += t->duration();
            album->setTotalDuration(total);
        }

        // Load album into queue and start playing
        if (!tracks.isEmpty()) {
            m_audioEngine->setQueue(tracks, "album_page", album->id());
            m_audioEngine->playAtIndex(0);
            m_queueHeader->setAlbum(album);
            m_tabWidget->setCurrentIndex(3); // Switch to "Now Playing" tab

            // Track current album for Last.fm scrobble count
            m_currentAlbumForScrobble = album;
            statusBar()->showMessage(QString("Playing album: %1 (%2 tracks)").arg(album->title()).arg(tracks.size()));
        } else {
            statusBar()->showMessage(QString("Album is empty: %1").arg(album->title()));
        }
    }
}

void MainWindow::onTrackReceived(std::shared_ptr<Track> track)
{
    if (!track) {
        emit debugLog("[MainWindow] onTrackReceived: track is null");
        return;
    }

    emit debugLog(QString("[MainWindow] Track received: %1 (ID: %2, Token: %3)")
                  .arg(track->title(), track->id(),
                       track->trackToken().isEmpty() ? "EMPTY" : "present"));

    // If we were waiting to play this track (e.g., from search results)
    if (m_pendingTrackPlayback) {
        m_pendingTrackPlayback = false;
        emit debugLog("[MainWindow] Playing fetched track");
        // Now that we have the full track details with TRACK_TOKEN, play it
        m_audioEngine->loadTrack(track);
        m_tabWidget->setCurrentIndex(3); // Switch to "Now Playing" tab
        statusBar()->showMessage("Playing: " + track->title() + " - " + track->artist());
    }
}

void MainWindow::onPlayClicked()
{
    m_audioEngine->play();
}

void MainWindow::onPauseClicked()
{
    m_audioEngine->pause();
}

void MainWindow::onStopClicked()
{
    m_audioEngine->stop();
}

void MainWindow::onNextClicked()
{
    m_audioEngine->next();
}

void MainWindow::onPreviousClicked()
{
    m_audioEngine->previous();
}

void MainWindow::onSeekRequested(double position)
{
    m_audioEngine->seek(position);
}

void MainWindow::onVolumeChanged(float volume)
{
    m_audioEngine->setVolume(volume);
}

void MainWindow::onError(const QString& error)
{
    statusBar()->showMessage("Error: " + error, 5000);
    QMessageBox::warning(this, "Error", error);
}

void MainWindow::onDebugLog(const QString& message)
{
    QString line = QString("[%1] %2")
        .arg(QDateTime::currentDateTime().toString(Qt::ISODate), message);
    
    m_debugLogLines.append(line);
    while (m_debugLogLines.size() > MAX_DEBUG_LINES)
        m_debugLogLines.removeFirst();
        
    // Auto-refresh the log window if it's open
    if (m_debugLogEdit) {
        m_debugLogEdit->appendPlainText(line);
        // Scroll to bottom
        m_debugLogEdit->verticalScrollBar()->setValue(m_debugLogEdit->verticalScrollBar()->maximum());
    }
}

void MainWindow::onViewDebugLog()
{
    if (m_debugLogDialog) {
        m_debugLogDialog->raise();
        m_debugLogDialog->activateWindow();
        return;
    }

    m_debugLogDialog = new QDialog(this);
    m_debugLogDialog->setAttribute(Qt::WA_DeleteOnClose);
    m_debugLogDialog->setWindowTitle("Debug log");
    m_debugLogDialog->setMinimumSize(800, 500);
    
    QVBoxLayout* layout = new QVBoxLayout(m_debugLogDialog);
    m_debugLogEdit = new QPlainTextEdit(m_debugLogDialog);
    m_debugLogEdit->setReadOnly(true);
    m_debugLogEdit->setPlainText(m_debugLogLines.isEmpty() ? "(No log entries yet.)" : m_debugLogLines.join("\n"));
    m_debugLogEdit->setFont(QFont("Consolas", 10));
    
    // Ensure we start at the bottom
    m_debugLogEdit->verticalScrollBar()->setValue(m_debugLogEdit->verticalScrollBar()->maximum());
    
    layout->addWidget(m_debugLogEdit);
    
    QPushButton* closeBtn = new QPushButton("Close", m_debugLogDialog);
    connect(closeBtn, &QPushButton::clicked, m_debugLogDialog, &QDialog::close);
    layout->addWidget(closeBtn);
    
    // Reset pointers when dialog is destroyed
    connect(m_debugLogDialog, &QObject::destroyed, this, [this]() {
        m_debugLogDialog = nullptr;
        m_debugLogEdit = nullptr;
    });
    
    m_debugLogDialog->show();
}

void MainWindow::updateLoginState()
{
    bool authenticated = m_deezerAPI->isAuthenticated();
    m_loginAction->setEnabled(!authenticated);
    m_logoutAction->setEnabled(authenticated);
}

void MainWindow::loadSettings()
{
    QSettings settings;

    // Restore window geometry
    QByteArray geometry = settings.value("Window/geometry").toByteArray();
    if (!geometry.isEmpty()) {
        restoreGeometry(geometry);
    }

    // Restore gapless playback preference
    bool gapless = settings.value("Preferences/gaplessPlayback", false).toBool();
    m_gaplessAction->setChecked(gapless);
    m_audioEngine->setGaplessEnabled(gapless);

    // Restore Discord RPC preference
    bool discordEnabled = settings.value("Preferences/discordRPC", false).toBool();
    m_discordRpcAction->setChecked(discordEnabled);
    QMetaObject::invokeMethod(m_discordManager, "setEnabled",
        Qt::QueuedConnection, Q_ARG(bool, discordEnabled));

    // Restore spectrum preference
    bool spectrum = settings.value("Preferences/spectrum", false).toBool();
    m_spectrumAction->setChecked(spectrum);
    // The onToggleSpectrumClicked will be called by the setChecked signal

    // Restore lyrics preference
    bool lyrics = settings.value("Preferences/lyrics", false).toBool();
    m_lyricsAction->setChecked(lyrics);
    // The onToggleLyricsClicked will be called by the setChecked signal

    emit debugLog(QString("[Settings] Loaded preferences: gapless=%1 discord=%2 spectrum=%3 lyrics=%4")
                  .arg(gapless ? "true" : "false")
                  .arg(discordEnabled ? "true" : "false")
                  .arg(spectrum ? "true" : "false")
                  .arg(lyrics ? "true" : "false"));

    // Load Last.fm settings
    QString lastFmApiKey = settings.value("LastFm/apiKey").toString();
    QString lastFmApiSecret = settings.value("LastFm/apiSecret").toString();
    QString lastFmSessionKey = settings.value("LastFm/sessionKey").toString();
    QString lastFmUsername = settings.value("LastFm/username").toString();

    if (!lastFmApiKey.isEmpty() && !lastFmApiSecret.isEmpty()) {
        m_lastFmAPI->setApiKey(lastFmApiKey);
        m_lastFmAPI->setApiSecret(lastFmApiSecret);

        if (!lastFmSessionKey.isEmpty() && !lastFmUsername.isEmpty()) {
            m_lastFmAPI->setSessionKey(lastFmSessionKey);
            m_lastFmAPI->setUsername(lastFmUsername);
            emit debugLog(QString("[LastFm] Restored session for user: %1").arg(lastFmUsername));
        }
    }
}

void MainWindow::saveSettings()
{
    QSettings settings;

    // Save window geometry
    settings.setValue("Window/geometry", saveGeometry());

    // Save preferences
    settings.setValue("Preferences/gaplessPlayback", m_gaplessAction->isChecked());
    settings.setValue("Preferences/discordRPC", m_discordRpcAction->isChecked());
    settings.setValue("Preferences/spectrum", m_spectrumAction->isChecked());
    settings.setValue("Preferences/lyrics", m_lyricsAction->isChecked());

    emit debugLog("[Settings] Saved all settings");
}

void MainWindow::autoLogin()
{
    QSettings settings;
    QString savedArl = settings.value("Authentication/arl").toString();

    if (savedArl.isEmpty()) {
        emit debugLog("[Settings] No saved credentials found");
        return;
    }

    emit debugLog("[Settings] Found saved ARL, attempting auto-login...");
    statusBar()->showMessage("Logging in with saved credentials...");

    // Use saved ARL to log in
    m_deezerAPI->signInWithArl(savedArl);
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    saveSettings();

    // Close and cleanup all child windows
    if (m_projectMWindow) {
        m_projectMWindow->close();
        m_projectMWindow->deleteLater();
        m_projectMWindow = nullptr;
    }

    if (m_spectrumWindow) {
        m_spectrumWindow->close();
        m_spectrumWindow->deleteLater();
        m_spectrumWindow = nullptr;
    }

    if (m_lyricsWindow) {
        m_lyricsWindow->close();
        m_lyricsWindow->deleteLater();
        m_lyricsWindow = nullptr;
    }

    if (m_debugLogDialog) {
        m_debugLogDialog->close();
        m_debugLogDialog->deleteLater();
        m_debugLogDialog = nullptr;
    }

    QMainWindow::closeEvent(event);
}

// Last.fm integration methods

void MainWindow::onLastFmSettingsClicked()
{
    LastFmSettingsDialog dialog(m_lastFmAPI, this);
    dialog.exec();

    // After settings changed, refetch scrobble data if authenticated
    if (m_lastFmAPI->isAuthenticated()) {
        m_scrobbleCache->clear();
        fetchScrobbleDataForQueue();
    }
}

void MainWindow::onLastFmAuthenticated(const QString& username)
{
    emit debugLog(QString("[LastFm] Authenticated as: %1").arg(username));

    // Fetch scrobble data for current queue
    fetchScrobbleDataForQueue();
}

void MainWindow::onLastFmTrackInfoReceived(const QString& trackKey, int playcount, int userPlaycount)
{
    // Update cache
    QStringList parts = trackKey.split('|');
    if (parts.size() != 2) return;

    QString artist = parts[0];
    QString track = parts[1];

    m_scrobbleCache->setTrackPlaycount(artist, track, playcount, userPlaycount);

    // Find matching track in queue widget's tracks and update it
    const QList<std::shared_ptr<Track>>& tracks = m_queueWidget->tracks();
    for (int i = 0; i < tracks.size(); ++i) {
        auto queueTrack = tracks[i];
        QString queueArtist = queueTrack->artist().toLower().trimmed();
        QString queueTitle = queueTrack->title().toLower().trimmed();

        if (queueArtist == artist && queueTitle == track) {
            queueTrack->setScrobbleCount(playcount);
            queueTrack->setUserScrobbleCount(userPlaycount);

            // Update UI with correct index
            m_queueWidget->updateTrackScrobbleCount(i);

            // Update album scrobble count (sum of all tracks in album)
            updateAlbumScrobbleCount();
            break;
        }
    }
}

void MainWindow::onLastFmAlbumInfoReceived(const QString& albumKey, int playcount, int userPlaycount)
{
    // Update cache (still cache album data for potential future use)
    QStringList parts = albumKey.split('|');
    if (parts.size() != 2) return;

    QString artist = parts[0];
    QString album = parts[1];

    m_scrobbleCache->setAlbumPlaycount(artist, album, playcount, userPlaycount);

    // Note: We now calculate album scrobbles by summing track scrobbles instead of using
    // Last.fm's album userplaycount (which only counts complete album plays)
}

void MainWindow::updateAlbumScrobbleCount()
{
    if (!m_currentAlbumForScrobble) {
        m_queueHeader->setAlbumScrobbleCount(-1);  // Hide scrobble count
        return;
    }

    // Sum up user scrobble counts from all tracks in the current album
    const QList<std::shared_ptr<Track>>& queue = m_audioEngine->queue();
    QString albumArtist = m_currentAlbumForScrobble->artist().toLower().trimmed();
    QString albumTitle = m_currentAlbumForScrobble->title().toLower().trimmed();

    int totalScrobbles = 0;
    int tracksWithData = 0;

    for (const auto& track : queue) {
        QString trackArtist = track->artist().toLower().trimmed();
        QString trackAlbum = track->album().toLower().trimmed();

        // Check if this track belongs to the current album
        if (trackArtist == albumArtist && trackAlbum == albumTitle) {
            if (track->hasScrobbleData()) {
                totalScrobbles += track->userScrobbleCount();
                tracksWithData++;
            }
        }
    }

    // Update the header with the sum
    if (tracksWithData > 0) {
        m_queueHeader->setAlbumScrobbleCount(totalScrobbles);
    } else {
        m_queueHeader->setAlbumScrobbleCount(-1);  // Hide if no data yet
    }
}

void MainWindow::fetchScrobbleDataForQueue()
{
    if (!m_lastFmAPI->isAuthenticated()) {
        return;
    }

    // Clear pending fetches
    m_pendingScrobbleFetches.clear();
    m_scrobbleFetchIndex = 0;

    const QList<std::shared_ptr<Track>>& queue = m_audioEngine->queue();

    // Collect tracks that need scrobble data fetched
    for (const auto& track : queue) {
        QString artist = track->artist().toLower().trimmed();
        QString title = track->title().toLower().trimmed();

        // Skip if already in cache
        if (m_scrobbleCache->hasTrackData(artist, title)) {
            ScrobbleData data = m_scrobbleCache->getTrackPlaycount(artist, title);
            if (data.isValid) {
                track->setScrobbleCount(data.playcount);
                track->setUserScrobbleCount(data.userPlaycount);
            }
            continue;
        }

        // Add to pending fetches
        m_pendingScrobbleFetches.append(QPair<QString, QString>(artist, title));
    }

    // Refresh queue widget to show cached data
    m_queueWidget->setTracks(queue);
    if (m_audioEngine->currentTrack()) {
        m_queueWidget->setCurrentTrackId(m_audioEngine->currentTrack()->id());
    }

    // Calculate and update album scrobble count from tracks
    updateAlbumScrobbleCount();

    // Start fetching pending tracks
    if (!m_pendingScrobbleFetches.isEmpty()) {
        emit debugLog(QString("[LastFm] Fetching scrobble data for %1 tracks").arg(m_pendingScrobbleFetches.size()));
        fetchNextBatchOfScrobbles();
    }
}

void MainWindow::fetchNextBatchOfScrobbles()
{
    if (m_scrobbleFetchIndex >= m_pendingScrobbleFetches.size()) {
        // All done
        emit debugLog("[LastFm] Finished fetching scrobble data");
        return;
    }

    // Fetch next batch (5 tracks at a time for rate limiting)
    int batchSize = qMin(5, m_pendingScrobbleFetches.size() - m_scrobbleFetchIndex);

    for (int i = 0; i < batchSize; ++i) {
        const auto& pair = m_pendingScrobbleFetches[m_scrobbleFetchIndex + i];
        m_lastFmAPI->getTrackInfo(pair.first, pair.second);
    }

    m_scrobbleFetchIndex += batchSize;

    // Schedule next batch with 1 second delay (rate limiting)
    if (m_scrobbleFetchIndex < m_pendingScrobbleFetches.size()) {
        m_scrobbleFetchTimer->start(1000);  // 1 second
    }
}

