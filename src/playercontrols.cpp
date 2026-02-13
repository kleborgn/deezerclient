#include "playercontrols.h"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QStyle>
#include <QEvent>

PlayerControls::PlayerControls(QWidget *parent)
    : QWidget(parent)
    , m_audioEngine(nullptr)
{
    // Create controls
    m_previousButton = new QPushButton(style()->standardIcon(QStyle::SP_MediaSkipBackward), "", this);
    m_playPauseButton = new QPushButton(style()->standardIcon(QStyle::SP_MediaPlay), "", this);
    m_stopButton = new QPushButton(style()->standardIcon(QStyle::SP_MediaStop), "", this);
    m_nextButton = new QPushButton(style()->standardIcon(QStyle::SP_MediaSkipForward), "", this);

    m_repeatButton = new QPushButton("Repeat: Off", this);
    m_repeatButton->setCheckable(true);

    // Install event filter on next button to detect hover
    m_nextButton->installEventFilter(this);

    m_waveformWidget = new WaveformWidget(this);

    m_volumeSlider = new QSlider(Qt::Horizontal, this);
    m_volumeSlider->setRange(0, 100);
    m_volumeSlider->setValue(80);
    m_volumeSlider->setMaximumWidth(100);

    m_trackInfoLabel = new QLabel("No track loaded", this);
    m_positionLabel = new QLabel("0:00", this);
    m_durationLabel = new QLabel("0:00", this);
    m_volumeLabel = new QLabel(QString::fromUtf8("\xF0\x9F\x94\x8A"), this);  // speaker emoji

    // Style track info label
    QFont infoFont = m_trackInfoLabel->font();
    infoFont.setPointSize(11);
    m_trackInfoLabel->setFont(infoFont);

    // Layout: 2 rows
    // Row 1: track info | transport buttons + volume | time
    // Row 2: full-width waveform
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(8, 4, 8, 4);
    mainLayout->setSpacing(4);

    QHBoxLayout* controlRow = new QHBoxLayout();
    controlRow->setSpacing(6);

    // Left: track info
    controlRow->addWidget(m_trackInfoLabel);
    controlRow->addStretch();

    // Center: transport buttons
    controlRow->addWidget(m_stopButton);
    controlRow->addWidget(m_previousButton);
    controlRow->addWidget(m_playPauseButton);
    controlRow->addWidget(m_nextButton);
    controlRow->addWidget(m_repeatButton);

    // Volume
    controlRow->addWidget(m_volumeLabel);
    controlRow->addWidget(m_volumeSlider);

    // Right: time labels
    controlRow->addWidget(m_positionLabel);
    controlRow->addWidget(m_durationLabel);

    mainLayout->addLayout(controlRow);
    mainLayout->addWidget(m_waveformWidget);

    // Connect signals
    connect(m_playPauseButton, &QPushButton::clicked, this, &PlayerControls::onPlayPauseClicked);
    connect(m_stopButton, &QPushButton::clicked, this, &PlayerControls::stopClicked);
    connect(m_nextButton, &QPushButton::clicked, this, &PlayerControls::nextClicked);
    connect(m_previousButton, &QPushButton::clicked, this, &PlayerControls::previousClicked);

    // Waveform seek
    connect(m_waveformWidget, &WaveformWidget::seekRequested, this, &PlayerControls::seekRequested);

    connect(m_volumeSlider, &QSlider::valueChanged, this, &PlayerControls::onVolumeSliderChanged);
    connect(m_repeatButton, &QPushButton::clicked, this, &PlayerControls::onRepeatClicked);
}

void PlayerControls::setAudioEngine(AudioEngine* engine)
{
    m_audioEngine = engine;

    if (m_audioEngine) {
        connect(m_audioEngine, &AudioEngine::stateChanged, this, &PlayerControls::onStateChanged);
        connect(m_audioEngine, &AudioEngine::trackChanged, this, &PlayerControls::onTrackChanged);
        connect(m_audioEngine, &AudioEngine::positionChanged, this, &PlayerControls::onPositionChanged);
        connect(m_audioEngine, &AudioEngine::positionTick, this, &PlayerControls::onPositionTick);
        connect(m_audioEngine, &AudioEngine::waveformReady, this, &PlayerControls::onWaveformReady);
        connect(m_audioEngine, &AudioEngine::repeatModeChanged, this, &PlayerControls::onRepeatModeChanged);

        // Internal state sync
        onRepeatModeChanged(m_audioEngine->repeatMode());

        // Set initial volume
        m_audioEngine->setVolume(m_volumeSlider->value() / 100.0f);
    }
}

void PlayerControls::onPlayPauseClicked()
{
    if (!m_audioEngine) {
        return;
    }

    if (m_audioEngine->state() == AudioEngine::Playing) {
        emit pauseClicked();
    } else {
        emit playClicked();
    }
}

void PlayerControls::onStateChanged(AudioEngine::PlaybackState state)
{
    Q_UNUSED(state);
    updatePlayPauseButton();
}

void PlayerControls::onTrackChanged(std::shared_ptr<Track> track)
{
    if (track) {
        // Format: "Artist   02.  Title"
        int trackNum = 0;
        if (m_audioEngine) trackNum = m_audioEngine->currentIndex() + 1;
        QString info = QString("%1   %2.  %3")
            .arg(track->artist())
            .arg(trackNum, 2, 10, QChar('0'))
            .arg(track->title());
        m_trackInfoLabel->setText(info);
        m_durationLabel->setText(track->durationString());
    } else {
        m_trackInfoLabel->setText("No track loaded");
        m_durationLabel->setText("0:00");
        m_waveformWidget->clear();
    }
}

void PlayerControls::onPositionChanged(int seconds)
{
    if (!m_waveformWidget->isDragging()) {
        m_positionLabel->setText(formatTime(seconds));
    }
}

void PlayerControls::onPositionTick(double position)
{
    m_waveformWidget->setPosition(position);
}

void PlayerControls::onWaveformReady(const QVector<float>& peaks)
{
    m_waveformWidget->setPeaks(peaks);
}

void PlayerControls::onVolumeSliderChanged(int value)
{
    emit volumeChanged(value / 100.0f);
}

void PlayerControls::updatePlayPauseButton()
{
    if (!m_audioEngine) {
        return;
    }

    if (m_audioEngine->state() == AudioEngine::Playing) {
        m_playPauseButton->setIcon(style()->standardIcon(QStyle::SP_MediaPause));
    } else {
        m_playPauseButton->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    }
}

QString PlayerControls::formatTime(int seconds) const
{
    int minutes = seconds / 60;
    int secs = seconds % 60;
    return QString("%1:%2").arg(minutes).arg(secs, 2, 10, QChar('0'));
}

void PlayerControls::onRepeatClicked()
{
    if (!m_audioEngine) return;

    AudioEngine::RepeatMode current = m_audioEngine->repeatMode();
    AudioEngine::RepeatMode nextMode = AudioEngine::RepeatOff;

    if (current == AudioEngine::RepeatOff) nextMode = AudioEngine::RepeatOne;
    else if (current == AudioEngine::RepeatOne) nextMode = AudioEngine::RepeatAll;
    else nextMode = AudioEngine::RepeatOff;

    m_audioEngine->setRepeatMode(nextMode);
}

void PlayerControls::onRepeatModeChanged(AudioEngine::RepeatMode mode)
{
    switch (mode) {
        case AudioEngine::RepeatOff:
            m_repeatButton->setText("Repeat: Off");
            m_repeatButton->setChecked(false);
            break;
        case AudioEngine::RepeatOne:
            m_repeatButton->setText("Repeat: One");
            m_repeatButton->setChecked(true);
            break;
        case AudioEngine::RepeatAll:
            m_repeatButton->setText("Repeat: All");
            m_repeatButton->setChecked(true);
            break;
    }
}

bool PlayerControls::eventFilter(QObject* watched, QEvent* event)
{
    // Detect when cursor enters the next button
    if (watched == m_nextButton && event->type() == QEvent::Enter) {
        // Trigger preloading when user hovers over next button
        if (m_audioEngine && !m_audioEngine->isNextPreloaded()) {
            m_audioEngine->preloadNextTrack();
        }
    }

    return QWidget::eventFilter(watched, event);
}
