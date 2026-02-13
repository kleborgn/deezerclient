#include "lyricswindow.h"
#include <QVBoxLayout>
#include <QSettings>

LyricsWindow::LyricsWindow(QWidget *parent)
    : QWidget(parent, Qt::Window)
    , m_audioEngine(nullptr)
{
    setWindowTitle("Lyrics - Deezer Client");
    resize(500, 600);

    // Don't prevent application from quitting when this window is still open
    setAttribute(Qt::WA_QuitOnClose, false);

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    m_lyricsWidget = new LyricsWidget(this);
    layout->addWidget(m_lyricsWidget);

    // Forward debug logs
    connect(m_lyricsWidget, &LyricsWidget::debugLog, this, &LyricsWindow::debugLog);

    // Restore window geometry if saved
    QSettings settings;
    QByteArray geometry = settings.value("LyricsWindow/geometry").toByteArray();
    if (!geometry.isEmpty()) {
        restoreGeometry(geometry);
    }
}

LyricsWindow::~LyricsWindow()
{
}

void LyricsWindow::setAudioEngine(AudioEngine* engine)
{
    m_audioEngine = engine;

    if (m_audioEngine) {
        // Connect to track changes
        connect(m_audioEngine, &AudioEngine::trackChanged,
                this, &LyricsWindow::onTrackChanged);

        // Connect to position updates for synchronization
        connect(m_audioEngine, &AudioEngine::positionChanged,
                this, &LyricsWindow::onPositionChanged);
    }
}

void LyricsWindow::onTrackChanged(std::shared_ptr<Track> track)
{
    if (!track) {
        m_lyricsWidget->clear();
        m_currentTrackId.clear();
        return;
    }

    m_currentTrackId = track->id();

    // Check if track already has lyrics cached
    if (!track->lyrics().isEmpty() || !track->syncedLyrics().isEmpty()) {
        m_lyricsWidget->setLyrics(track->lyrics(), track->syncedLyrics());
    } else {
        // Show loading message
        m_lyricsWidget->clear();
        // Lyrics will be fetched by MainWindow and delivered via updateLyrics()
    }
}

void LyricsWindow::onPositionChanged(int seconds)
{
    m_lyricsWidget->setPosition(seconds);
}

void LyricsWindow::updateLyrics(const QString& trackId, const QString& lyrics, const QJsonArray& syncedLyrics)
{
    // Only update if this is for the current track
    if (trackId == m_currentTrackId) {
        m_lyricsWidget->setLyrics(lyrics, syncedLyrics);
    }
}

void LyricsWindow::closeEvent(QCloseEvent* event)
{
    // Save geometry before closing
    QSettings settings;
    settings.setValue("LyricsWindow/geometry", saveGeometry());

    emit closed();
    QWidget::closeEvent(event);
}
