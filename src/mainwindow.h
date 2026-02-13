#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QSplitter>
#include <QMenuBar>
#include <QStatusBar>
#include <QThread>
#include <QSettings>
#include <QCloseEvent>
#include <QPainter>
#include <memory>
#include "audioengine.h"
#include "deezerapi.h"
#include "playercontrols.h"
#include "tracklistwidget.h"
#include "playlistwidget.h"
#include "albumlistwidget.h"
#include "searchwidget.h"
#include "discordmanager.h"
#include "queueheaderwidget.h"
#include "projectmwindow.h"

class QDialog;
class QPlainTextEdit;
class QTimer;
class LastFmAPI;
class ScrobbleCache;

class AspectRatioLabel : public QLabel
{
    Q_OBJECT
public:
    explicit AspectRatioLabel(QWidget* parent = nullptr) : QLabel(parent) {
        setMinimumSize(100, 100);
    }

    void setPixmap(const QPixmap& p) {
        m_originalPixmap = p;
        update();
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QLabel::resizeEvent(event);
        update();
    }

    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);

        if (m_originalPixmap.isNull()) {
            // Draw placeholder text
            painter.fillRect(rect(), QColor(30, 30, 30));
            painter.setPen(QColor(128, 128, 128));
            painter.drawText(rect(), Qt::AlignCenter, text());
            return;
        }

        // Scale to fill (KeepAspectRatioByExpanding) and center-crop
        QPixmap scaled = m_originalPixmap.scaled(size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        int x = (width() - scaled.width()) / 2;
        int y = (height() - scaled.height()) / 2;
        painter.drawPixmap(x, y, scaled);
    }

private:
    QPixmap m_originalPixmap;
};

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    // Menu actions
    void onLoginClicked();
    void onLogoutClicked();
    void onQuitClicked();
    void onAboutClicked();
    void onToggleGaplessClicked(bool checked);
    void onToggleDiscordRpcClicked(bool checked);
    void onToggleSpectrumClicked(bool checked);
    void onToggleLyricsClicked(bool checked);
    void onLastFmSettingsClicked();

    // Authentication
    void onAuthenticated(const QString& username);
    void onAuthenticationFailed(const QString& error);
    
    // Playback
    void onTrackDoubleClicked(std::shared_ptr<Track> track);
    void onTracksSelected(QList<std::shared_ptr<Track>> tracks);
    void onPlaylistDoubleClicked(std::shared_ptr<Playlist> playlist);
    void onPlaylistReceived(std::shared_ptr<Playlist> playlist);
    void onAlbumDoubleClicked(std::shared_ptr<Album> album);
    void onAlbumReceived(std::shared_ptr<Album> album, QList<std::shared_ptr<Track>> songs);
    void onTrackReceived(std::shared_ptr<Track> track);

    // Player controls
    void onPlayClicked();
    void onPauseClicked();
    void onStopClicked();
    void onNextClicked();
    void onPreviousClicked();
    void onSeekRequested(double position);
    void onVolumeChanged(float volume);

    // Lyrics
    void onLyricsReceived(const QString& trackId, const QString& lyrics, const QJsonArray& syncedLyrics);

    // Error handling
    void onError(const QString& error);
    void onDebugLog(const QString& message);
    void onViewDebugLog();

    // Last.fm
    void onLastFmTrackInfoReceived(const QString& trackKey, int playcount, int userPlaycount);
    void onLastFmAlbumInfoReceived(const QString& albumKey, int playcount, int userPlaycount);
    void onLastFmAuthenticated(const QString& username);
    void fetchScrobbleDataForQueue();
    void fetchNextBatchOfScrobbles();
    void updateAlbumScrobbleCount();

signals:
    void debugLog(const QString& message);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void setupUI();
    void setupMenus();
    void createConnections();
    void updateLoginState();
    void loadSettings();
    void saveSettings();
    void autoLogin();
    
    // Core components
    AudioEngine* m_audioEngine;
    DeezerAPI* m_deezerAPI;
    DiscordManager* m_discordManager;
    QThread* m_discordThread;
    
    // UI Widgets
    QTabWidget* m_tabWidget;

    // Library tab widgets
    PlaylistWidget* m_playlistWidget;
    AlbumListWidget* m_albumWidget;
    SearchWidget* m_searchWidget;
    TrackListWidget* m_trackListWidget;
    PlayerControls* m_libraryPlayerControls;
    
    // Now Playing tab widgets
    AspectRatioLabel* m_largeAlbumArtLabel;
    TrackListWidget* m_queueWidget;
    QueueHeaderWidget* m_queueHeader;
    PlayerControls* m_nowPlayingPlayerControls;
    
    // Menus
    QMenu* m_fileMenu;
    QMenu* m_settingsMenu;
    QMenu* m_helpMenu;
    QAction* m_loginAction;
    QAction* m_logoutAction;
    QAction* m_gaplessAction;
    QAction* m_discordRpcAction;
    QAction* m_spectrumAction;
    QAction* m_lyricsAction;
    QAction* m_lastFmSettingsAction;

    // Current state
    QList<std::shared_ptr<Track>> m_currentQueue;
    QStringList m_debugLogLines;
    static const int MAX_DEBUG_LINES = 500;
    bool m_pendingTrackPlayback = false;  // Flag to play track after fetching details
    
    QDialog* m_debugLogDialog = nullptr;
    QPlainTextEdit* m_debugLogEdit = nullptr;

    QNetworkAccessManager* m_imageManager;

    class SpectrumWindow* m_spectrumWindow = nullptr;
    class LyricsWindow* m_lyricsWindow = nullptr;
    ProjectMWindow* m_projectMWindow = nullptr;

    // Last.fm integration
    class LastFmAPI* m_lastFmAPI = nullptr;
    class ScrobbleCache* m_scrobbleCache = nullptr;
    QTimer* m_scrobbleFetchTimer = nullptr;
    QList<QPair<QString, QString>> m_pendingScrobbleFetches;  // (artist, track) pairs to fetch
    int m_scrobbleFetchIndex = 0;
    std::shared_ptr<Album> m_currentAlbumForScrobble;  // Track current album for header scrobble count
};

#endif // MAINWINDOW_H
