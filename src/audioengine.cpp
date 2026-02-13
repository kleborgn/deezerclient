#include "audioengine.h"
#include "deezerapi.h"
#include "streamdownloader.h"
#include "windowsmediacontrols.h"
#include <QTimer>
#include <QThread>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrl>
#include <QMetaObject>
#include <QtConcurrent>
#include <QFutureWatcher>

// Include BASS headers
// You'll need to download bass.h and bassmix.h from un4seen.com
extern "C" {
#include "bass.h"
#include "bassmix.h"
}

AudioEngine::AudioEngine(QObject *parent)
    : QObject(parent)
    , m_mixerStream(0)
    , m_currentStream(0)
    , m_queueSync(0)
    , m_currentStreamAdded(false)
    , m_currentEndSync(0)
    , m_currentNearEndSync(0)
    , m_state(Stopped)
    , m_volume(0.8f)
    , m_gaplessEnabled(true)
    , m_initialized(false)
    , m_lastPositionSeconds(-1)
    , m_downloadThread(nullptr)
    , m_streamDownloader(nullptr)
    , m_networkManager(nullptr)
    , m_downloadReply(nullptr)
    , m_preloadDownloader(nullptr)
    , m_downloadTimeoutTimer(nullptr)
    , m_deezerAPI(nullptr)
    , m_currentIndex(-1)
    , m_repeatMode(RepeatOff)
    , m_preloadStream(0)
{
    qRegisterMetaType<DWORD>("DWORD");

    m_positionTimer = new QTimer(this);
    connect(m_positionTimer, &QTimer::timeout, this, &AudioEngine::updatePosition);

    m_spectrumTimer = new QTimer(this);
    m_spectrumTimer->setInterval(33); // ~30 FPS
    m_spectrumEnabled = true;  // Enabled by default for visualizer support
    connect(m_spectrumTimer, &QTimer::timeout, this, &AudioEngine::updateSpectrum);

    m_networkManager = new QNetworkAccessManager(this);
    connect(m_networkManager, &QNetworkAccessManager::finished, this, &AudioEngine::onDownloadFinished);
    m_downloadTimeoutTimer = new QTimer(this);
    m_downloadTimeoutTimer->setSingleShot(true);
    connect(m_downloadTimeoutTimer, &QTimer::timeout, this, &AudioEngine::onDownloadTimeout);

    m_downloadThread = new QThread(this);
    m_streamDownloader = new StreamDownloader();
    m_streamDownloader->moveToThread(m_downloadThread);
    connect(m_streamDownloader, &StreamDownloader::downloadReady, this, &AudioEngine::onDownloadDataReady, Qt::QueuedConnection);

    m_preloadDownloader = new StreamDownloader();
    m_preloadDownloader->moveToThread(m_downloadThread);
    connect(m_preloadDownloader, &StreamDownloader::downloadReady, this, &AudioEngine::onDownloadDataReady, Qt::QueuedConnection);

    m_downloadThread->start();

    m_windowsMediaControls = new WindowsMediaControls(this);
    connect(m_windowsMediaControls, &WindowsMediaControls::playRequested, this, &AudioEngine::play);
    connect(m_windowsMediaControls, &WindowsMediaControls::pauseRequested, this, &AudioEngine::pause);
    connect(m_windowsMediaControls, &WindowsMediaControls::nextRequested, this, &AudioEngine::next);
    connect(m_windowsMediaControls, &WindowsMediaControls::previousRequested, this, &AudioEngine::previous);
}

AudioEngine::~AudioEngine()
{
    shutdown();
}

bool AudioEngine::initialize()
{
    if (m_initialized) {
        return true;
    }

    emit debugLog("[AudioEngine] initialize() called");

    // Initialize BASS
    // -1 = default device, 44100 Hz, stereo, no special flags
    if (!BASS_Init(-1, 44100, 0, nullptr, nullptr)) {
        emit error("Failed to initialize BASS audio library");
        return false;
    }

    // Create mixer stream (persistent, 44.1kHz stereo)
    m_mixerStream = BASS_Mixer_StreamCreate(
        44100,
        2,
        BASS_SAMPLE_FLOAT | BASS_MIXER_QUEUE | BASS_MIXER_RESUME
    );

    if (!m_mixerStream) {
        int err = BASS_ErrorGetCode();
        emit error(QString("Failed to create mixer stream: error %1").arg(err));
        BASS_Free();
        return false;
    }

    // Set up BASS_SYNC_MIXER_QUEUE sync to be notified when streams are dequeued
    m_queueSync = BASS_ChannelSetSync(
        m_mixerStream,
        BASS_SYNC_MIXER_QUEUE,
        0,
        queueSyncCallback,
        this
    );

    if (!m_queueSync) {
        int err = BASS_ErrorGetCode();
        emit debugLog(QString("[AudioEngine] ⚠️ Failed to set BASS_SYNC_MIXER_QUEUE: error %1").arg(err));
    } else {
        emit debugLog("[AudioEngine] ✓ BASS_SYNC_MIXER_QUEUE sync set");
    }

    // Set volume on mixer
    BASS_ChannelSetAttribute(m_mixerStream, BASS_ATTRIB_VOL, m_volume);

    // Start spectrum timer if enabled
    if (m_spectrumEnabled) {
        emit debugLog("[AudioEngine] Starting spectrum timer...");
        m_spectrumTimer->start();
        emit debugLog(QString("[AudioEngine] Spectrum timer started: %1").arg(m_spectrumTimer->isActive() ? "ACTIVE" : "INACTIVE"));
    }

    m_initialized = true;
    emit debugLog("[AudioEngine] Initialized with BassMix gapless playback");
    return true;
}

void AudioEngine::shutdown()
{
    if (!m_initialized) {
        return;
    }

    stop();
    destroyStream();

    QMutexLocker locker(&m_bassMutex);
    if (m_mixerStream) {
        BASS_StreamFree(m_mixerStream);
        m_mixerStream = 0;
    }
    locker.unlock();

    if (m_downloadThread && m_downloadThread->isRunning()) {
        m_downloadThread->quit();
        m_downloadThread->wait(2000);
    }
    BASS_Free();
    m_initialized = false;
}

void AudioEngine::play()
{
    if (!m_initialized || !m_mixerStream) {
        emit debugLog("[AudioEngine] play() ignored: not initialized or no mixer");
        return;
    }

    QMutexLocker locker(&m_bassMutex);

    BOOL ok = BASS_ChannelPlay(m_mixerStream, FALSE);
    if (ok) {
        setState(Playing);
        m_positionTimer->start(100);
        emit debugLog("[AudioEngine] Playing mixer stream");
    } else {
        emit debugLog(QString("[AudioEngine] BASS_ChannelPlay(mixer) failed: %1").arg(BASS_ErrorGetCode()));
    }
}

void AudioEngine::pause()
{
    if (!m_initialized || !m_mixerStream) {
        return;
    }

    QMutexLocker locker(&m_bassMutex);
    BASS_ChannelPause(m_mixerStream);
    setState(Paused);
    m_positionTimer->stop();
}

void AudioEngine::stop()
{
    if (!m_initialized || !m_mixerStream) {
        return;
    }

    m_positionTimer->stop();

    QMutexLocker locker(&m_bassMutex);

    // Remove current stream syncs
    if (m_currentStream) {
        if (m_currentEndSync) {
            BASS_ChannelRemoveSync(m_currentStream, m_currentEndSync);
            m_currentEndSync = 0;
        }
        if (m_currentNearEndSync) {
            BASS_ChannelRemoveSync(m_currentStream, m_currentNearEndSync);
            m_currentNearEndSync = 0;
        }
        BASS_Mixer_ChannelRemove(m_currentStream);
        BASS_StreamFree(m_currentStream);
        m_currentStream = 0;
    }

    // Stop mixer - this will automatically remove all queued sources
    BASS_ChannelStop(m_mixerStream);
    setState(Stopped);
}

void AudioEngine::seek(double position)
{
    if (!m_initialized || !m_mixerStream) {
        return;
    }

    QMutexLocker locker(&m_bassMutex);
    if (!m_currentStream) {
        return;
    }

    QWORD length = BASS_ChannelGetLength(m_currentStream, BASS_POS_BYTE);
    QWORD seekPos = static_cast<QWORD>(length * position);

    BASS_Mixer_ChannelSetPosition(m_currentStream, seekPos,
                                   BASS_POS_BYTE | BASS_POS_MIXER_RESET);
}

void AudioEngine::setDeezerAPI(DeezerAPI* api)
{
    m_deezerAPI = api;
}

void AudioEngine::loadTrack(std::shared_ptr<Track> track)
{
    if (!m_initialized || !track) {
        return;
    }
    if (!m_deezerAPI) {
        emit error("Cannot play: API not configured");
        setState(Stopped);
        return;
    }
    if (track->trackToken().isEmpty()) {
        emit error("Login required to play full tracks");
        setState(Stopped);
        return;
    }

    setState(Loading);
    if (m_downloadReply) {
        QNetworkReply* oldReply = m_downloadReply;
        m_downloadReply = nullptr;
        oldReply->abort();
        oldReply->deleteLater();
    }
    destroyStream();
    m_listenReported = false;
    ++m_waveformGeneration;
    emit waveformReady(QVector<float>());

    m_currentTrack = track;

    // If the next track was preloaded, use the already-decrypted data directly
    if (m_preloadReady && m_preloadTrack && m_preloadTrack->id() == track->id()) {
        emit debugLog("[AudioEngine] Using preloaded data for: " + track->title());
        m_currentStreamFormat = m_preloadFormat;
        m_streamBuffer = m_preloadBuffer;  // Already decrypted — do NOT decrypt again
        m_preloadTrack.reset();
        m_preloadReady = false;
        m_preloadBuffer.clear();
        m_preloadStream = 0;

        HSTREAM newStream = createSourceStream(m_streamBuffer);
        if (!newStream) {
            setState(Stopped);
            return;
        }

        {
            QMutexLocker locker(&m_bassMutex);
            emit debugLog(QString("[AudioEngine] Adding preloaded stream %1 to mixer").arg(newStream));
            BOOL ok = BASS_Mixer_StreamAddChannel(m_mixerStream, newStream, 0);
            if (!ok) {
                int err = BASS_ErrorGetCode();
                emit error(QString("Failed to add stream to mixer: error %1").arg(err));
                BASS_StreamFree(newStream);
                setState(Stopped);
                return;
            }
            m_currentStream = newStream;
            setupStreamSyncs(m_currentStream, &m_currentEndSync, &m_currentNearEndSync);

            // Restore mixer queue sync (removed by destroyStream)
            if (!m_queueSync && m_mixerStream) {
                m_queueSync = BASS_ChannelSetSync(m_mixerStream, BASS_SYNC_MIXER_QUEUE, 0, queueSyncCallback, this);
            }

            DWORD mixerActive = BASS_ChannelIsActive(m_mixerStream);
            if (mixerActive != BASS_ACTIVE_PLAYING) {
                BASS_ChannelPlay(m_mixerStream, FALSE);
            }
        }

        // Stream info
        {
            QMutexLocker locker(&m_bassMutex);
            BASS_CHANNELINFO ci = {};
            if (BASS_ChannelGetInfo(m_currentStream, &ci)) {
                int bitrate = 0;
                double duration = BASS_ChannelBytes2Seconds(m_currentStream,
                    BASS_ChannelGetLength(m_currentStream, BASS_POS_BYTE));
                if (duration > 0 && m_streamBuffer.size() > 0)
                    bitrate = static_cast<int>((static_cast<double>(m_streamBuffer.size()) * 8.0) / (duration * 1000.0));
                QString chanStr = ci.chans == 1 ? "mono" : ci.chans == 2 ? "stereo" : QString("%1ch").arg(ci.chans);
                QString fmt = m_currentStreamFormat.isEmpty() ? "unknown" : m_currentStreamFormat;
                QString info = QString("%1 | %2 kbps | %3 Hz | %4").arg(fmt).arg(bitrate).arg(ci.freq).arg(chanStr);
                emit streamInfoChanged(info);
                emit debugLog(QString("[AudioEngine] Stream: %1").arg(info));
            }
        }

        startWaveformComputation();
        emit trackChanged(m_currentTrack);
        if (m_windowsMediaControls && m_currentTrack)
            m_windowsMediaControls->updateMetadata(m_currentTrack->title(), m_currentTrack->artist(), m_currentTrack->album(), m_currentTrack->albumArt());
        play();
        return;
    }

    // Clear any stale preload
    m_preloadTrack.reset();
    m_preloadReady = false;
    m_preloadBuffer.clear();
    m_preloadStream = 0;  // Clear preloaded stream reference

    m_pendingTrack = track;
    m_deezerAPI->getStreamUrl(track->id(), track->trackToken());

    // DON'T preload here - preload happens when we're near the end of current track
    // The near-end sync (setupStreamSyncs) will trigger preloadNextTrack() at the right time
}

void AudioEngine::onStreamUrlReceived(const QString& trackId, const QString& url, const QString& format)
{
    // ── Handle preload URL ──────────────────────────────────────────
    if (m_preloadTrack && m_preloadTrack->id() == trackId) {
        if (url.contains("cdns-preview", Qt::CaseInsensitive)) {
            emit debugLog("[AudioEngine] Preload: preview URL, skipping");
            m_preloadTrack.reset();
            return;
        }
        m_preloadFormat = format;
        // Defensive: cache title before async call in case m_preloadTrack is cleared
        QString trackTitle = m_preloadTrack->title();
        emit debugLog(QString("[AudioEngine] Preload URL received for: %1 (format: %2)")
                       .arg(trackTitle).arg(format));
        if (url.startsWith("https://", Qt::CaseInsensitive)) {
            QMetaObject::invokeMethod(m_preloadDownloader, "startDownload", Qt::QueuedConnection,
                                      Q_ARG(QString, url), Q_ARG(QString, trackId));
        }
        return;
    }

    // ── Normal path for pending track ───────────────────────────────
    if (!m_pendingTrack || m_pendingTrack->id() != trackId) {
        return;
    }
    if (url.contains("cdns-preview", Qt::CaseInsensitive)) {
        m_pendingTrack.reset();
        emit error("Full track not available. Please log in and try again.");
        setState(Stopped);
        return;
    }
    m_currentTrack = m_pendingTrack;
    m_pendingTrack.reset();
    m_currentStreamFormat = format;
    emit debugLog(QString("[AudioEngine] Full stream URL received (format: %1)").arg(format));
    startLoadingUrl(url);
}

void AudioEngine::startLoadingUrl(const QString& url)
{
    if (url.isEmpty()) {
        emit error("No stream URL available");
        setState(Stopped);
        return;
    }
    if (url.startsWith("https://", Qt::CaseInsensitive)) {
        QString trackId = m_currentTrack ? m_currentTrack->id() : QString();
        emit debugLog("[AudioEngine] Downloading full track (HTTPS) in worker thread: " + url);
        QMetaObject::invokeMethod(m_streamDownloader, "startDownload", Qt::QueuedConnection,
                                  Q_ARG(QString, url), Q_ARG(QString, trackId));
        return;
    }
    if (!createStream(url)) {
        setState(Stopped);
        return;
    }
    emit trackChanged(m_currentTrack);
    play();
}

void AudioEngine::setQueue(const QList<std::shared_ptr<Track>>& tracks)
{
    m_queue = tracks;
    m_currentIndex = -1;
    m_contextType.clear();
    m_contextId.clear();
    emit queueChanged();
}

void AudioEngine::setQueue(const QList<std::shared_ptr<Track>>& tracks, const QString& contextType, const QString& contextId)
{
    m_queue = tracks;
    m_currentIndex = -1;
    m_contextType = contextType;
    m_contextId = contextId;
    emit queueChanged();
}

QList<std::shared_ptr<Track>> AudioEngine::queue() const
{
    return m_queue;
}

void AudioEngine::playAtIndex(int index)
{
    if (index >= 0 && index < m_queue.size()) {
        m_currentIndex = index;
        loadTrack(m_queue[m_currentIndex]);
        emit queueChanged();
    }
}

void AudioEngine::next()
{
    if (m_queue.isEmpty()) {
        stop();
        return;
    }

    int nextIndex = m_currentIndex + 1;
    if (nextIndex >= m_queue.size()) {
        if (m_repeatMode == RepeatAll) {
            nextIndex = 0;
        } else {
            stop();
            return;
        }
    }

    // Load next track
    m_currentIndex = nextIndex;
    loadTrack(m_queue[m_currentIndex]);
    emit queueChanged();
}

void AudioEngine::previous()
{
    if (m_queue.isEmpty()) return;

    int prevIndex = m_currentIndex - 1;
    if (prevIndex < 0) {
        if (m_repeatMode == RepeatAll) {
            prevIndex = m_queue.size() - 1;
        } else {
            seek(0.0);
            return;
        }
    }

    m_currentIndex = prevIndex;
    loadTrack(m_queue[m_currentIndex]);
    emit queueChanged();
}

void AudioEngine::setRepeatMode(RepeatMode mode)
{
    if (m_repeatMode != mode) {
        m_repeatMode = mode;
        emit repeatModeChanged(m_repeatMode);
    }
}

void AudioEngine::setVolume(float volume)
{
    m_volume = qBound(0.0f, volume, 1.0f);

    if (m_initialized && m_mixerStream) {
        BASS_ChannelSetAttribute(m_mixerStream, BASS_ATTRIB_VOL, m_volume);
    }

    emit volumeChanged(m_volume);
}

double AudioEngine::position() const
{
    QMutexLocker locker(&m_bassMutex);
    if (!m_initialized || !m_mixerStream || !m_currentStream) {
        return 0.0;
    }

    QWORD pos = BASS_Mixer_ChannelGetPosition(m_currentStream, BASS_POS_BYTE);
    QWORD length = BASS_ChannelGetLength(m_currentStream, BASS_POS_BYTE);

    if (length == 0) {
        return 0.0;
    }

    return static_cast<double>(pos) / static_cast<double>(length);
}

int AudioEngine::positionSeconds() const
{
    QMutexLocker locker(&m_bassMutex);
    
    // Check if initialized and mixer exists
    if (!m_initialized || !m_mixerStream) {
        return 0;
    }
    
    // Try to get position from current stream (if set)
    if (m_currentStream) {
        // Verify stream is still valid and active in mixer
        DWORD active = BASS_Mixer_ChannelIsActive(m_currentStream);
        if (active != BASS_ACTIVE_PLAYING && active != BASS_ACTIVE_PAUSED) {
            return 0;
        }
        
        QWORD pos = BASS_Mixer_ChannelGetPosition(m_currentStream, BASS_POS_BYTE);
        
        // Check for error conditions
        if (pos == (QWORD)-1) {
            return 0;
        }
        
        double seconds = BASS_ChannelBytes2Seconds(m_currentStream, pos);
        
        // Ensure non-negative result
        if (seconds < 0.0) {
            return 0;
        }
        
        return static_cast<int>(seconds);
    }
    
    // Fallback: try to get position from mixer directly
    // This works when m_currentStream hasn't been set yet (during transitions)
    QWORD pos = BASS_ChannelGetPosition(m_mixerStream, BASS_POS_BYTE);
    
    if (pos == (QWORD)-1) {
        return 0;
    }
    
    QWORD length = BASS_ChannelGetLength(m_mixerStream, BASS_POS_BYTE);
    if (length == 0 || length == (QWORD)-1) {
        return 0;
    }
    
    double seconds = BASS_ChannelBytes2Seconds(m_mixerStream, pos);
    
    // Ensure non-negative result
    if (seconds < 0.0) {
        return 0;
    }
    
    return static_cast<int>(seconds);
}

int AudioEngine::durationSeconds() const
{
    QMutexLocker locker(&m_bassMutex);
    if (!m_initialized || !m_currentStream) {
        return 0;
    }

    QWORD length = BASS_ChannelGetLength(m_currentStream, BASS_POS_BYTE);
    double seconds = BASS_ChannelBytes2Seconds(m_currentStream, length);
    
    return static_cast<int>(seconds);
}

void AudioEngine::updatePosition()
{
    int currentSeconds = positionSeconds();
    
    if (currentSeconds != m_lastPositionSeconds) {
        m_lastPositionSeconds = currentSeconds;
        emit positionChanged(currentSeconds);

        // Report play to Deezer after 30 seconds
        if (currentSeconds >= 30 && !m_listenReported && m_currentTrack) {
            m_listenReported = true;
            QString trackId = m_currentTrack->id();
            QString trackTitle = m_currentTrack->title();
            int duration = durationSeconds();
            if (duration <= 0) duration = m_currentTrack->duration();

            m_deezerAPI->reportListen(trackId, duration, m_currentStreamFormat, m_contextType, m_contextId);
            QString contextInfo = (!m_contextType.isEmpty() && !m_contextId.isEmpty())
                ? QString(" [Context: %1/%2]").arg(m_contextType, m_contextId)
                : QString();
            emit debugLog(QString("[AudioEngine] Play reported to Deezer for: %1 (ID: %2, Dur: %3s, Fmt: %4%5)")
                           .arg(trackTitle).arg(trackId).arg(duration).arg(m_currentStreamFormat).arg(contextInfo));
        }
    }

    // Fine-grained position for smooth waveform playhead (~10 updates/sec)
    emit positionTick(position());
}

void AudioEngine::setSpectrumEnabled(bool enabled)
{
    m_spectrumEnabled = enabled;

    if (enabled) {
        m_spectrumTimer->start();
    } else {
        m_spectrumTimer->stop();
    }
}

void AudioEngine::updateSpectrum()
{
    static int callCount = 0;
    callCount++;

    if (!m_spectrumEnabled || !m_mixerStream) {
        return;
    }

    QMutexLocker locker(&m_bassMutex);

    // Get PCM data from mixer output (512 samples per channel for 30fps visualizer)
    static constexpr int PCM_SAMPLES = 512;
    static std::vector<float> pcmLeft(PCM_SAMPLES);
    static std::vector<float> pcmRight(PCM_SAMPLES);
    static float pcmInterleaved[PCM_SAMPLES * 2];

    DWORD result = BASS_ChannelGetData(m_mixerStream, pcmInterleaved, BASS_DATA_FLOAT | PCM_SAMPLES * 2 * sizeof(float));

    if (result == (DWORD)-1) {
        if (callCount <= 3) {
            emit debugLog(QString("[AudioEngine] BASS_ChannelGetData failed, error code: %1")
                         .arg(BASS_ErrorGetCode()));
        }
        return; // No data or error
    }

    // Deinterleave PCM data (L R L R L R -> LLL..., RRR...)
    int samplesRetrieved = result / (2 * sizeof(float));
    samplesRetrieved = qMin(samplesRetrieved, PCM_SAMPLES);
    
    for (int i = 0; i < samplesRetrieved; ++i) {
        pcmLeft[i] = pcmInterleaved[i * 2];      // Left channel
        pcmRight[i] = pcmInterleaved[i * 2 + 1]; // Right channel
    }

    // Fill remaining with zeros if needed
    for (int i = samplesRetrieved; i < PCM_SAMPLES; ++i) {
        pcmLeft[i] = 0.0f;
        pcmRight[i] = 0.0f;
    }

    // Get FFT data for spectrum
    float fft[4096];
    result = BASS_ChannelGetData(m_mixerStream, fft, BASS_DATA_FFT8192);

    // Convert FFT bins to 32 frequency bands (logarithmic grouping)
    const int numBands = 32;
    QVector<float> magnitudes(numBands, 0.0f);

    if (result != (DWORD)-1) {
        // Logarithmic frequency distribution
        for (int band = 0; band < numBands; ++band) {
            // Calculate bin range for this band
            float lowFreq = 20.0f * qPow(2.0f, band * qLn(20000.0f / 20.0f) / (numBands * qLn(2.0f)));
            float highFreq = 20.0f * qPow(2.0f, (band + 1) * qLn(20000.0f / 20.0f) / (numBands * qLn(2.0f)));

            int startBin = static_cast<int>(lowFreq * 4096 / 22050.0f);
            int endBin = static_cast<int>(highFreq * 4096 / 22050.0f);

            if (endBin > 4096) endBin = 4096;
            if (startBin >= endBin) startBin = endBin - 1;

            // Average magnitude across bins in this band
            float sum = 0.0f;
            for (int bin = startBin; bin < endBin; ++bin) {
                sum += fft[bin];
            }
            float avg = sum / (endBin - startBin);

            // Apply scaling
            magnitudes[band] = qBound(0.0f, avg * 50.0f, 1.0f);
        }
    }

    emit spectrumDataReady(magnitudes);
    emit pcmDataReady(pcmLeft, pcmRight);
}

void AudioEngine::setState(PlaybackState state)
{
    if (m_state != state) {
        m_state = state;
        emit stateChanged(state);

        if (m_windowsMediaControls) {
            m_windowsMediaControls->updatePlaybackState(state == Playing);
        }
    }
}

static QString bassErrorString(int code)
{
    switch (code) {
    case 1:  return "memory error";
    case 2:  return "could not open URL/file";
    case 3:  return "driver error";
    case 6:  return "unsupported format";
    case 8:  return "BASS not initialized";
    case 9:  return "playback start failed";
    case 10: return "SSL/HTTPS not available (need BASS addon or libssl)";
    case 32: return "no network connection";
    case 40: return "timeout";
    case 47: return "URL not streamable";
    case 48: return "unsupported protocol";
    default: return QString("error code %1").arg(code);
    }
}

bool AudioEngine::createStream(const QString& url)
{
    emit debugLog("[AudioEngine] Creating stream from URL: " + url);

    // Create stream from URL with DECODE flag (required for mixer)
    {
        QMutexLocker locker(&m_bassMutex);
        m_currentStream = BASS_StreamCreateURL(
            url.toUtf8().constData(),
            0,
            BASS_STREAM_DECODE | BASS_STREAM_BLOCK | BASS_STREAM_STATUS,
            nullptr,
            nullptr
        );
    }

    if (!m_currentStream) {
        int err = BASS_ErrorGetCode();
        QString msg = QString("Failed to load track: %1").arg(bassErrorString(err));
        emit debugLog("[AudioEngine] " + msg);
        emit error(msg);
        return false;
    }

    setupStreamSyncs(m_currentStream, &m_currentEndSync, &m_currentNearEndSync);

    // Restore mixer queue sync (removed by destroyStream)
    if (!m_queueSync && m_mixerStream) {
        m_queueSync = BASS_ChannelSetSync(m_mixerStream, BASS_SYNC_MIXER_QUEUE, 0, queueSyncCallback, this);
    }

    return true;
}

HSTREAM AudioEngine::createSourceStream(const QByteArray& data)
{
    if (data.isEmpty()) {
        emit error("Failed to load track: empty data");
        return 0;
    }

    // Create decode stream (not for direct playback, will be added to mixer)
    QMutexLocker locker(&m_bassMutex);
    HSTREAM stream = BASS_StreamCreateFile(
        TRUE,
        data.constData(),
        0,
        static_cast<QWORD>(data.size()),
        BASS_STREAM_DECODE | BASS_SAMPLE_FLOAT  // DECODE flag crucial for mixer!
    );

    if (!stream) {
        int err = BASS_ErrorGetCode();
        QString msg = QString("Failed to create stream: %1").arg(bassErrorString(err));
        emit debugLog("[AudioEngine] " + msg);
        emit error(msg);
        return 0;
    }

    return stream;
}

void AudioEngine::setupStreamSyncs(HSTREAM stream, HSYNC* endSyncPtr, HSYNC* nearEndSyncPtr)
{
    QMutexLocker locker(&m_bassMutex);
    if (!stream) return;

    QWORD length = BASS_ChannelGetLength(stream, BASS_POS_BYTE);
    double lengthSeconds = BASS_ChannelBytes2Seconds(stream, length);
    // Preload 30s before end to allow time for slow network downloads
    double preloadLeadTime = qMin(30.0, lengthSeconds * 0.5);  // At most half the track
    QWORD nearEndPos = length - BASS_ChannelSeconds2Bytes(stream, preloadLeadTime);
    double nearEndSeconds = BASS_ChannelBytes2Seconds(stream, nearEndPos);

    // Near-end sync (for preloading) - ONETIME flag auto-removes sync after firing
    if (nearEndSyncPtr) {
        *nearEndSyncPtr = BASS_Mixer_ChannelSetSync(
            stream,
            BASS_SYNC_POS | BASS_SYNC_MIXTIME | BASS_SYNC_ONETIME,
            nearEndPos,
            syncNearEndCallback,
            this
        );
        if (*nearEndSyncPtr) {
            emit debugLog(QString("[AudioEngine] Set NEAR_END sync on stream %1: will fire at %2s (track length: %3s)")
                         .arg(stream)
                         .arg(nearEndSeconds, 0, 'f', 2)
                         .arg(lengthSeconds, 0, 'f', 2));
        } else {
            int err = BASS_ErrorGetCode();
            emit debugLog(QString("[AudioEngine] Failed to set NEAR_END sync on stream %1: error %2")
                         .arg(stream).arg(err));
        }
    }

    // End sync (for detecting when stream finishes and promoting next stream)
    // ONETIME flag auto-removes sync after firing, preventing issues with freeing stream in callback
    if (endSyncPtr) {
        *endSyncPtr = BASS_Mixer_ChannelSetSync(
            stream,
            BASS_SYNC_END | BASS_SYNC_MIXTIME | BASS_SYNC_ONETIME,
            0,
            syncEndCallback,
            this
        );
        if (*endSyncPtr) {
            emit debugLog(QString("[AudioEngine] Set END sync on stream %1 (length: %2s)")
                         .arg(stream)
                         .arg(lengthSeconds, 0, 'f', 2));
        } else {
            int err = BASS_ErrorGetCode();
            emit debugLog(QString("[AudioEngine] Failed to set END sync on stream %1: error %2")
                         .arg(stream).arg(err));
        }
    }
}

void AudioEngine::setupStreamSync(HSTREAM stream, HSYNC* syncPtr)
{
    QMutexLocker locker(&m_bassMutex);
    if (!stream || !syncPtr) return;

    // End sync only
    *syncPtr = BASS_Mixer_ChannelSetSync(
        stream,
        BASS_SYNC_END | BASS_SYNC_MIXTIME,
        0,
        syncEndCallback,
        this
    );
}

void AudioEngine::addStreamToMixer(const QByteArray& data)
{
    QMutexLocker locker(&m_bassMutex);
    
    // Create decode stream
    HSTREAM stream = createSourceStream(data);
    if (!stream) {
        emit error("Failed to create source stream from data");
        return;
    }
    
    // Add to mixer with queuing flags
    BOOL ok = BASS_Mixer_StreamAddChannel(
        m_mixerStream,
        stream,
        BASS_MIXER_CHAN_NORAMPIN | BASS_STREAM_AUTOFREE
    );
    
    if (!ok) {
        int err = BASS_ErrorGetCode();
        emit debugLog(QString("[AudioEngine] Failed to add stream to mixer: %1").arg(err));
        BASS_StreamFree(stream);
        return;
    }
    
    // Update current stream for position tracking
    m_currentStream = stream;
    
    // Update stream info
    locker.unlock();
    updateStreamInfo(stream);
    locker.relock();
}

void AudioEngine::updateStreamInfo(HSTREAM stream)
{
    if (!stream) return;
    
    QMutexLocker locker(&m_bassMutex);
    
    BASS_CHANNELINFO ci = {};
    if (BASS_ChannelGetInfo(stream, &ci)) {
        int bitrate = 0;
        double duration = BASS_ChannelBytes2Seconds(stream, BASS_ChannelGetLength(stream, BASS_POS_BYTE));
        if (duration > 0 && m_streamBuffer.size() > 0)
            bitrate = static_cast<int>((static_cast<double>(m_streamBuffer.size()) * 8.0) / (duration * 1000.0));
        QString chanStr = ci.chans == 1 ? "mono" : ci.chans == 2 ? "stereo" : QString("%1ch").arg(ci.chans);
        QString fmt = m_currentStreamFormat.isEmpty() ? "unknown" : m_currentStreamFormat;
        QString info = QString("%1 | %2 kbps | %3 Hz | %4").arg(fmt).arg(bitrate).arg(ci.freq).arg(chanStr);
        emit streamInfoChanged(info);
        emit debugLog(QString("[AudioEngine] Stream: %1").arg(info));
    }
}

void AudioEngine::handleStreamDequeued(DWORD streamHandle, int generation)
{
    // Discard stale callbacks from a previous playback session
    if (generation != m_dequeueGeneration) {
        emit debugLog(QString("[AudioEngine] Ignoring stale dequeue for stream %1 (gen %2, current %3)")
                     .arg(streamHandle).arg(generation).arg(m_dequeueGeneration));
        return;
    }

    emit debugLog(QString("[AudioEngine] Stream %1 dequeued from mixer (current: %2)")
                 .arg(streamHandle).arg(m_currentStream));

    QMutexLocker locker(&m_bassMutex);

    // BASS_SYNC_MIXER_QUEUE fires when a source STARTS playing (dequeued from waiting queue).
    // If it matches m_currentStream, this is the initial activation — ignore it.
    if (streamHandle == m_currentStream) {
        emit debugLog(QString("[AudioEngine] Stream %1 is current stream (initial activation, ignoring)")
                     .arg(streamHandle));
        return;
    }

    // A different stream started playing — this is a gapless track transition.
    HSTREAM oldStream = m_currentStream;
    emit debugLog(QString("[AudioEngine] Track transition: stream %1 → %2, advancing queue from index %3 to %4")
                 .arg(oldStream).arg(streamHandle)
                 .arg(m_currentIndex).arg(m_currentIndex + 1));

    m_currentStream = streamHandle;
    m_preloadStream = 0;
    m_currentIndex++;

    // Wrap around for RepeatAll mode
    if (m_currentIndex >= m_queue.size() && m_repeatMode == RepeatAll) {
        m_currentIndex = 0;
        emit debugLog("[AudioEngine] Wrapped m_currentIndex to 0 (RepeatAll)");
    }

    // Free the finished stream BEFORE overwriting its backing buffer.
    // The old stream still references m_streamBuffer's raw memory
    // (BASS_StreamCreateFile with mem=TRUE). Must free it first.
    if (oldStream) {
        BASS_Mixer_ChannelRemove(oldStream);
        BASS_StreamFree(oldStream);
    }

    // Now safe to replace the buffer — no BASS stream references the old data.
    m_streamBuffer = m_preloadBuffer;
    m_preloadBuffer.clear();

    // Set up syncs on the new current stream (preloaded streams don't have them)
    m_currentEndSync = 0;
    m_currentNearEndSync = 0;
    setupStreamSyncs(m_currentStream, &m_currentEndSync, &m_currentNearEndSync);

    // Reset listen-reported flag for the new track
    m_listenReported = false;

    // Check bounds and get next track (make copy to avoid issues after unlock)
    std::shared_ptr<Track> nextTrack;
    int queueSize = m_queue.size();

    if (m_currentIndex < queueSize) {
        nextTrack = m_queue[m_currentIndex];
        m_currentTrack = nextTrack;

        emit debugLog(QString("[AudioEngine] Next track set to: %1 (index %2/%3)")
                     .arg(nextTrack ? nextTrack->title() : "Unknown")
                     .arg(m_currentIndex + 1)
                     .arg(queueSize));
    }

    m_lastPositionSeconds = -1;  // Force position update

    // IMPORTANT: Unlock BEFORE emitting signals to avoid deadlocks
    locker.unlock();

    if (nextTrack) {
        emit debugLog(QString("[AudioEngine] About to emit trackChanged signal for track: %1 (duration: %2s)")
                     .arg(nextTrack->title())
                     .arg(nextTrack->duration()));

        // Emit signals to update UI
        emit trackChanged(nextTrack);
        emit trackEnded();

        emit debugLog(QString("[AudioEngine] trackChanged signal emitted"));

        // Preload next track
        preloadNextTrack();
    } else {
        emit debugLog("[AudioEngine] Reached end of queue");
        m_currentTrack.reset();
        emit trackChanged(nullptr);
        emit trackEnded();
    }
}

void AudioEngine::onDownloadTimeout()
{
    if (m_downloadReply) {
        emit debugLog("[AudioEngine] Download timeout (120s), aborting");
        m_downloadReply->abort();
    }
}

void AudioEngine::onDownloadDataReady(const QByteArray& data, const QString& errorMessage, const QString& trackId)
{
    // ── Handle preload download completion ───────────────────────────
    if (m_preloadTrack && m_preloadTrack->id() == trackId) {
        if (errorMessage.isEmpty() && !data.isEmpty()) {
            m_preloadBuffer = data;
            m_preloadReady = true;

            // Decrypt preload buffer
            if (m_deezerAPI && m_preloadTrack && !m_preloadTrack->id().isEmpty()) {
                if (m_deezerAPI->decryptStreamBuffer(m_preloadBuffer, m_preloadTrack->id())) {
                    emit debugLog("[AudioEngine] Decrypted preload stream (BF_CBC_STRIPE)");
                }
            }

            // Create source stream and ADD TO MIXER immediately
            // With BASS_MIXER_QUEUE flag, it will wait until current finishes
            HSTREAM nextStream = createSourceStream(m_preloadBuffer);
            if (nextStream) {
                QMutexLocker locker(&m_bassMutex);

                // Verify mixer is valid before queuing
                if (!m_mixerStream) {
                    emit debugLog("[AudioEngine] ERROR: Cannot queue - mixer stream is null");
                    BASS_StreamFree(nextStream);
                    return;
                }

                // Add to mixer - will automatically play after current stream ends
                BOOL ok = BASS_Mixer_StreamAddChannel(
                    m_mixerStream,
                    nextStream,
                    BASS_MIXER_CHAN_NORAMPIN | BASS_STREAM_AUTOFREE
                );
                
                if (!ok) {
                    int err = BASS_ErrorGetCode();
                    emit debugLog(QString("[AudioEngine] Failed to add next stream to mixer: %1").arg(err));
                    BASS_StreamFree(nextStream);
                    locker.unlock();
                    return;
                }

                // Store the preloaded stream handle so we can set it as current when track changes
                m_preloadStream = nextStream;

                emit debugLog(QString("[AudioEngine] Next track queued in mixer: %1").arg(nextStream));
                
                // Store track title
                QString trackTitle = m_preloadTrack ? m_preloadTrack->title() : "Unknown";

                // Unlock mutex before emitting signals
                locker.unlock();

                emit debugLog(QString("[AudioEngine] Next track ready for gapless playback: %1").arg(trackTitle));
            } else {
                emit debugLog("[AudioEngine] ERROR: createSourceStream returned null for preload");
            }
        } else if (errorMessage.contains("cancel", Qt::CaseInsensitive)) {
            // Silently ignore cancelled preload downloads
            emit debugLog("[AudioEngine] Preload download cancelled");
            m_preloadTrack.reset();
        } else {
            emit debugLog("[AudioEngine] Preload download failed: " + errorMessage);
            m_preloadTrack.reset();
        }
        return;
    }

    // ── Normal path ─────────────────────────────────────────────────
    if (!m_currentTrack || m_currentTrack->id() != trackId) {
        emit debugLog("[AudioEngine] Ignoring stale download (track changed)");
        return;
    }
    if (!errorMessage.isEmpty()) {
        // Ignore "operation cancelled" errors - these happen when switching tracks quickly
        if (errorMessage.contains("cancel", Qt::CaseInsensitive)) {
            emit debugLog("[AudioEngine] Download cancelled (expected when switching tracks)");
            return;
        }
        emit debugLog("[AudioEngine] " + errorMessage);
        emit error(QString("Failed to load track: %1").arg(errorMessage));
        setState(Stopped);
        return;
    }
    m_streamBuffer = data;
    if (m_streamBuffer.isEmpty()) {
        emit error("Failed to load track: empty response from server");
        setState(Stopped);
        return;
    }
    emit debugLog(QString("[AudioEngine] Downloaded %1 bytes, creating stream").arg(m_streamBuffer.size()));
    // Full streams from get_url are BF_CBC_STRIPE encrypted; decrypt in-place if TRACK_XOR_KEY is set
    if (m_deezerAPI && m_currentTrack && !m_currentTrack->id().isEmpty()) {
        if (m_deezerAPI->decryptStreamBuffer(m_streamBuffer, m_currentTrack->id()))
            emit debugLog("[AudioEngine] Decrypted stream (BF_CBC_STRIPE)");
        else
            emit debugLog("[AudioEngine] TRACK_XOR_KEY not set or decryption skipped - stream may be corrupted");
    }
    // Sanity check: avoid treating HTML error pages as audio
    if (m_streamBuffer.size() >= 4) {
        const char* p = m_streamBuffer.constData();
        bool looksLikeAudio =
            (p[0] == (char)0xFF && (p[1] & 0xE0) == 0xE0) ||  // MP3 frame sync
            (p[0] == 'I' && p[1] == 'D' && p[2] == '3') ||    // ID3 tag
            (p[0] == 'f' && p[1] == 'L' && p[2] == 'a' && p[3] == 'C'); // FLAC
        if (!looksLikeAudio) {
            emit debugLog("[AudioEngine] Warning: downloaded data may not be audio. First bytes: "
                + QString::number((quint8)p[0]) + "," + QString::number((quint8)p[1]) + "," + QString::number((quint8)p[2]) + "," + QString::number((quint8)p[3]));
        }
    }

    // Create source stream
    HSTREAM newStream = createSourceStream(m_streamBuffer);
    if (!newStream) {
        setState(Stopped);
        return;
    }

    QMutexLocker locker(&m_bassMutex);

    // Remove old current stream syncs and free stream
    if (m_currentStream) {
        if (m_currentEndSync) {
            BASS_ChannelRemoveSync(m_currentStream, m_currentEndSync);
            m_currentEndSync = 0;
        }
        if (m_currentNearEndSync) {
            BASS_ChannelRemoveSync(m_currentStream, m_currentNearEndSync);
            m_currentNearEndSync = 0;
        }
        BASS_Mixer_ChannelRemove(m_currentStream);
        BASS_StreamFree(m_currentStream);
    }

    // Add to mixer (plays immediately)
    emit debugLog(QString("[AudioEngine] Adding stream %1 to mixer (plays immediately, flags=0)...").arg(newStream));
    BOOL ok = BASS_Mixer_StreamAddChannel(m_mixerStream, newStream, 0);
    if (!ok) {
        int err = BASS_ErrorGetCode();
        emit error(QString("Failed to add stream to mixer: error %1").arg(err));
        BASS_StreamFree(newStream);
        setState(Stopped);
        return;
    }

    m_currentStream = newStream;
    emit debugLog(QString("[AudioEngine] Stream %1 added to mixer").arg(newStream));

    // Setup syncs AFTER adding to mixer (BASS_Mixer_ChannelSetSync requires stream to be in mixer)
    setupStreamSyncs(m_currentStream, &m_currentEndSync, &m_currentNearEndSync);

    // Restore mixer queue sync (removed by destroyStream)
    if (!m_queueSync && m_mixerStream) {
        m_queueSync = BASS_ChannelSetSync(m_mixerStream, BASS_SYNC_MIXER_QUEUE, 0, queueSyncCallback, this);
    }

    // Start mixer if not already playing
    DWORD mixerActive = BASS_ChannelIsActive(m_mixerStream);
    if (mixerActive != BASS_ACTIVE_PLAYING) {
        emit debugLog(QString("[AudioEngine] Starting mixer (was not playing, state=%1)").arg(mixerActive));
        BASS_ChannelPlay(m_mixerStream, FALSE);
    } else {
        emit debugLog("[AudioEngine] Mixer already playing");
    }

    locker.unlock();
    // Emit stream info (format, bitrate, sample rate, channels)
    {
        QMutexLocker locker(&m_bassMutex);
        BASS_CHANNELINFO ci = {};
        if (BASS_ChannelGetInfo(m_currentStream, &ci)) {
            int bitrate = 0;
            double duration = BASS_ChannelBytes2Seconds(m_currentStream, BASS_ChannelGetLength(m_currentStream, BASS_POS_BYTE));
            if (duration > 0 && m_streamBuffer.size() > 0)
                bitrate = static_cast<int>((static_cast<double>(m_streamBuffer.size()) * 8.0) / (duration * 1000.0));
            QString chanStr = ci.chans == 1 ? "mono" : ci.chans == 2 ? "stereo" : QString("%1ch").arg(ci.chans);
            QString fmt = m_currentStreamFormat.isEmpty() ? "unknown" : m_currentStreamFormat;
            QString info = QString("%1 | %2 kbps | %3 Hz | %4").arg(fmt).arg(bitrate).arg(ci.freq).arg(chanStr);
            emit streamInfoChanged(info);
            emit debugLog(QString("[AudioEngine] Stream: %1").arg(info));
        }
    }
    // Compute waveform peaks in background thread (non-blocking)
    startWaveformComputation();

    emit trackChanged(m_currentTrack);
    
    if (m_windowsMediaControls && m_currentTrack) {
        m_windowsMediaControls->updateMetadata(m_currentTrack->title(), m_currentTrack->artist(), m_currentTrack->album(), m_currentTrack->albumArt());
    }

    play();
}

void AudioEngine::onDownloadFinished()
{
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply || reply != m_downloadReply) return;
    m_downloadTimeoutTimer->stop();
    m_downloadReply = nullptr;
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        QString msg = QString("Failed to load track: %1").arg(reply->errorString());
        emit debugLog("[AudioEngine] " + msg);
        emit error(msg);
        setState(Stopped);
        return;
    }
    QByteArray data = reply->readAll();
    onDownloadDataReady(data, QString(), m_currentTrack ? m_currentTrack->id() : QString());
}

void AudioEngine::destroyStream()
{
    QMutexLocker locker(&m_bassMutex);
    
    // Cancel any pending downloads
    if (m_downloadReply) {
        QNetworkReply* oldReply = m_downloadReply;
        m_downloadReply = nullptr;
        oldReply->abort();
        oldReply->deleteLater();
    }
    
    // Remove queue sync BEFORE stopping, so dequeuing sources won't fire stale callbacks
    if (m_queueSync && m_mixerStream) {
        BASS_ChannelRemoveSync(m_mixerStream, m_queueSync);
        m_queueSync = 0;
    }

    // Bump generation so any already-queued callbacks are ignored
    m_dequeueGeneration++;

    // Stop mixer FIRST so BASS is no longer decoding from any source
    if (m_mixerStream) {
        BASS_ChannelStop(m_mixerStream);
    }

    // Now safely remove and free all source streams.
    // Stopping alone does NOT remove sources — they stay attached and
    // would play again when the mixer is restarted.
    if (m_currentStream) {
        BASS_Mixer_ChannelRemove(m_currentStream);
        BASS_StreamFree(m_currentStream);
    }
    if (m_preloadStream) {
        BASS_Mixer_ChannelRemove(m_preloadStream);
        BASS_StreamFree(m_preloadStream);
    }
    m_currentEndSync = 0;
    m_currentNearEndSync = 0;

    // Clear references
    m_currentStream = 0;
    m_preloadStream = 0;
    m_streamBuffer.clear();
    
    // Cancel worker downloads
    QMetaObject::invokeMethod(m_streamDownloader, "startDownload", Qt::QueuedConnection,
                              Q_ARG(QString, QString()), Q_ARG(QString, QString()));
    QMetaObject::invokeMethod(m_preloadDownloader, "startDownload", Qt::QueuedConnection,
                              Q_ARG(QString, QString()), Q_ARG(QString, QString()));
}

void AudioEngine::handleStreamEnd(DWORD streamHandle)
{
    // This callback is no longer needed with BASS_SYNC_MIXER_QUEUE
    // Track transitions are handled by the queue sync callback (handleStreamDequeued)
    emit debugLog(QString("[AudioEngine] END sync fired for stream %1 (now handled by BASS_SYNC_MIXER_QUEUE)")
                 .arg(streamHandle));
}

void AudioEngine::handleNearEnd()
{
    emit debugLog(QString("[AudioEngine] NEAR_END sync fired! Current stream: %1, position: %2s")
                 .arg(m_currentStream)
                 .arg(positionSeconds()));

    if (!m_gaplessEnabled) {
        emit debugLog("[AudioEngine] Gapless disabled, skipping preload");
        return;
    }

    // Trigger preloading
    emit debugLog("[AudioEngine] Triggering preloadNextTrack()...");
    preloadNextTrack();
}

void AudioEngine::handleMixerEnd()
{
    // Not used in this implementation - mixer handles transitions via source-level syncs
    emit debugLog("[AudioEngine] Mixer ended (not used)");
}

void AudioEngine::preloadNextTrack()
{
    emit debugLog("[AudioEngine] preloadNextTrack() called - START");

    int nextIndex = m_currentIndex + 1;
    emit debugLog(QString("[AudioEngine] Current index: %1, next index: %2, queue size: %3")
                 .arg(m_currentIndex)
                 .arg(nextIndex)
                 .arg(m_queue.size()));

    if (nextIndex >= m_queue.size()) {
        if (m_repeatMode == RepeatAll) {
            nextIndex = 0;
            emit debugLog("[AudioEngine] Preload: wrapping to queue start (RepeatAll mode)");
        } else {
            emit debugLog("[AudioEngine] Preload: reached end of queue, no more tracks");
            return;
        }
    }

    emit debugLog(QString("[AudioEngine] Accessing queue at index %1 (queue size: %2)...").arg(nextIndex).arg(m_queue.size()));

    // Double-check bounds before accessing (queue could have changed)
    if (nextIndex < 0 || nextIndex >= m_queue.size()) {
        emit debugLog(QString("[AudioEngine] ERROR: Invalid queue index %1 (queue size: %2)").arg(nextIndex).arg(m_queue.size()));
        return;
    }

    auto nextTrack = m_queue[nextIndex]; // peek, don't dequeue
    emit debugLog(QString("[AudioEngine] Queue access successful, track pointer: %1").arg((qulonglong)nextTrack.get()));

    // Validate the track pointer before using it
    if (!nextTrack) {
        emit debugLog("[AudioEngine] Preload skipped: null track in queue");
        return;
    }

    emit debugLog(QString("[AudioEngine] Track title: '%1'").arg(nextTrack->title()));

    if (nextTrack->trackToken().isEmpty()) {
        emit debugLog(QString("[AudioEngine] Preload skipped: track '%1' has no token").arg(nextTrack->title()));
        return;
    }

    emit debugLog("[AudioEngine] Checking for duplicates...");
    // Don't preload if matches current, pending, or already preloading/preloaded
    if ((m_currentTrack && m_currentTrack->id() == nextTrack->id()) ||
        (m_pendingTrack && m_pendingTrack->id() == nextTrack->id()) ||
        (m_preloadTrack && m_preloadTrack->id() == nextTrack->id()) ||
        (m_preloadStream != 0)) {
        emit debugLog(QString("[AudioEngine] Preload skipped: track '%1' matches current, pending, or already preloaded").arg(nextTrack->title()));
        return;
    }

    emit debugLog("[AudioEngine] Setting preload track...");
    m_preloadTrack  = nextTrack;
    m_preloadReady  = false;
    m_preloadBuffer.clear();
    emit debugLog("[AudioEngine] Preload track set successfully");

    emit debugLog(QString("[AudioEngine] Starting preload for track %1/%2: '%3' (id: %4)")
                 .arg(nextIndex + 1)
                 .arg(m_queue.size())
                 .arg(nextTrack->title())
                 .arg(nextTrack->id()));

    if (m_deezerAPI) {
        emit debugLog("[AudioEngine] Calling getStreamUrl on DeezerAPI...");
        m_deezerAPI->getStreamUrl(nextTrack->id(), nextTrack->trackToken());
        emit debugLog("[AudioEngine] getStreamUrl call completed");
    } else {
        emit debugLog("[AudioEngine] ERROR: DeezerAPI is null!");
    }

    emit debugLog("[AudioEngine] preloadNextTrack() - END");
}

void CALLBACK AudioEngine::syncEndCallback(HSYNC handle, DWORD channel, DWORD data, void* user)
{
    AudioEngine* engine = static_cast<AudioEngine*>(user);
    if (engine) {
        QMetaObject::invokeMethod(engine, "handleStreamEnd", Qt::QueuedConnection,
                                  Q_ARG(DWORD, channel));
    }
}

void CALLBACK AudioEngine::syncNearEndCallback(HSYNC handle, DWORD channel, DWORD data, void* user)
{
    AudioEngine* engine = static_cast<AudioEngine*>(user);
    if (engine) {
        QMetaObject::invokeMethod(engine, "handleNearEnd", Qt::QueuedConnection);
    }
}

void CALLBACK AudioEngine::mixerEndCallback(HSYNC handle, DWORD channel, DWORD data, void* user)
{
    AudioEngine* engine = static_cast<AudioEngine*>(user);
    if (engine) {
        QMetaObject::invokeMethod(engine, "handleMixerEnd", Qt::QueuedConnection);
    }
}

void CALLBACK AudioEngine::queueSyncCallback(HSYNC handle, DWORD channel, DWORD data, void* user)
{
    AudioEngine* engine = static_cast<AudioEngine*>(user);
    if (engine) {
        // Capture generation now so stale callbacks can be discarded
        int gen = engine->m_dequeueGeneration;
        QMetaObject::invokeMethod(engine, "handleStreamDequeued", Qt::QueuedConnection,
                                  Q_ARG(DWORD, data), Q_ARG(int, gen));
    }
}

// ── Waveform computation (runs on thread-pool, never blocks the UI) ─────────

// Thread-safe free function: takes a copy of the audio audio buffer (QByteArray COW)
// and returns normalised peak amplitudes. Only uses its own BASS decode handle.
// Aborts if currentGeneration != *generationPtr
static QVector<float> computeWaveformFromBuffer(const QByteArray& data, int numPeaks,
                                               std::atomic<int>* generationPtr, int currentGeneration,
                                               QRecursiveMutex* bassMutex)
{
    QVector<float> peaks;
    if (data.isEmpty() || numPeaks <= 0)
        return peaks;

    HSTREAM decode = 0;
    {
        QMutexLocker locker(bassMutex);
        decode = BASS_StreamCreateFile(
            TRUE,
            data.constData(),
            0,
            static_cast<QWORD>(data.size()),
            BASS_STREAM_DECODE | BASS_SAMPLE_FLOAT
        );
    }
    if (!decode)
        return peaks;

    QWORD totalBytes = BASS_ChannelGetLength(decode, BASS_POS_BYTE);
    if (totalBytes == 0 || totalBytes == static_cast<QWORD>(-1)) {
        QMutexLocker locker(bassMutex);
        BASS_StreamFree(decode);
        return peaks;
    }

    peaks.resize(numPeaks);
    const QWORD bytesPerPeak = totalBytes / numPeaks;

    static constexpr int BUF_SAMPLES = 8192;
    float buffer[BUF_SAMPLES];

    for (int i = 0; i < numPeaks; ++i) {
        float peak = 0.0f;
        QWORD remaining = bytesPerPeak;
        float sum = 0.0f;
        int count = 0;
        QWORD remainingSegmentBytes = bytesPerPeak;

        while (remainingSegmentBytes > 0) {
            // Early abort check: if generation changed, user skipped to a new track
            if (generationPtr && generationPtr->load() != currentGeneration) {
                QMutexLocker locker(bassMutex);
                BASS_StreamFree(decode);
                return QVector<float>(); 
            }

            DWORD toRead = static_cast<DWORD>(
                qMin(static_cast<QWORD>(BUF_SAMPLES * sizeof(float)), remainingSegmentBytes));
            
            DWORD bytesRead = 0;
            {
                QMutexLocker locker(bassMutex);
                bytesRead = BASS_ChannelGetData(decode, buffer, toRead);
            }

            if (bytesRead == static_cast<DWORD>(-1) || bytesRead == 0)
                break; // error, end-of-stream, or no data

            int samples = static_cast<int>(bytesRead / sizeof(float));
            for (int s = 0; s < samples; ++s) {
                sum += qAbs(buffer[s]);
            }
            count += samples;
            remainingSegmentBytes -= bytesRead;
        }
        peaks[i] = (count > 0) ? (sum / count) : 0.0f;
    }

    {
        QMutexLocker locker(bassMutex);
        BASS_StreamFree(decode);
    }

    // Normalise peaks to 0.0 – 1.0
    float maxPeak = 0.0f;
    for (float p : peaks)
        if (p > maxPeak) maxPeak = p;

    if (maxPeak > 0.0f) {
        for (float& p : peaks) {
            p /= maxPeak;
            // Apply a power transform to increase visual dynamic range.
            // pow(p, 1.5) or pow(p, 2.0) makes quiet parts smaller and loud parts stand out,
            // avoiding the "blocky" look on compressed modern tracks.
            p = std::pow(p, 1.5f);
        }
    }

    return peaks;
}

void AudioEngine::startWaveformComputation()
{
    if (m_streamBuffer.isEmpty())
        return;

    const int generation = m_waveformGeneration.load();
    // QByteArray is copy-on-write: the worker keeps a cheap reference to the
    // buffer.  If loadTrack() clears m_streamBuffer before the worker reads,
    // the COW copy detaches safely – no data race.
    QByteArray bufferSnapshot = m_streamBuffer;

    auto *watcher = new QFutureWatcher<QVector<float>>(this);
    connect(watcher, &QFutureWatcher<QVector<float>>::finished, this,
            [this, watcher, generation]() {
        // Only accept the result if no newer track has been loaded since we started
        if (generation == m_waveformGeneration.load()) {
            QVector<float> peaks = watcher->result();
            if (!peaks.isEmpty()) {
                emit waveformReady(peaks);
                emit debugLog(QString("[AudioEngine] Waveform computed: %1 peaks").arg(peaks.size()));
            }
        }
        watcher->deleteLater();
    });

    watcher->setFuture(QtConcurrent::run(computeWaveformFromBuffer, bufferSnapshot, 500, &m_waveformGeneration, generation, &m_bassMutex));
}

// ── Queue Management Methods ────────────────────────────────────────────

void AudioEngine::removeFromQueue(int index)
{
    if (index < 0 || index >= m_queue.size())
        return;

    // Case 1: Removing track before current → decrement m_currentIndex
    if (index < m_currentIndex) {
        m_queue.removeAt(index);
        m_currentIndex--;
        emit queueChanged();
        return;
    }

    // Case 2: Removing current track → stop and restart playback
    if (index == m_currentIndex) {
        bool wasPlaying = (m_state == Playing);
        stop();  // Clean up BASS streams
        m_queue.removeAt(index);

        // Try to continue playback with next track
        if (!m_queue.isEmpty() && m_currentIndex < m_queue.size()) {
            loadTrack(m_queue[m_currentIndex]);
            if (wasPlaying)
                play();
        } else {
            m_currentIndex = -1;
            m_currentTrack.reset();
        }

        emit queueChanged();
        return;
    }

    // Case 3: Removing track after current
    // Check if it's the preloaded track
    if (m_preloadTrack && m_queue[index]->id() == m_preloadTrack->id()) {
        // Cancel preload
        m_preloadTrack.reset();
        m_preloadReady = false;
        m_preloadBuffer.clear();
        m_preloadStream = 0;
        // Cancel worker download (empty URL cancels)
        QMetaObject::invokeMethod(m_preloadDownloader, "startDownload",
                                  Qt::QueuedConnection,
                                  Q_ARG(QString, QString()),
                                  Q_ARG(QString, QString()));
    }

    m_queue.removeAt(index);
    emit queueChanged();
}

void AudioEngine::removeFromQueue(const QList<int>& indices)
{
    if (indices.isEmpty())
        return;

    // Sort descending to preserve indices during removal
    QList<int> sorted = indices;
    std::sort(sorted.begin(), sorted.end(), std::greater<int>());

    bool removingCurrent = sorted.contains(m_currentIndex);

    if (removingCurrent) {
        bool wasPlaying = (m_state == Playing);
        stop();

        for (int index : sorted) {
            if (index >= 0 && index < m_queue.size()) {
                m_queue.removeAt(index);
                if (index < m_currentIndex)
                    m_currentIndex--;
            }
        }

        // Resume playback if possible
        if (!m_queue.isEmpty() && m_currentIndex >= 0 &&
            m_currentIndex < m_queue.size()) {
            loadTrack(m_queue[m_currentIndex]);
            if (wasPlaying)
                play();
        } else {
            m_currentIndex = -1;
            m_currentTrack.reset();
        }
    } else {
        for (int index : sorted) {
            if (index >= 0 && index < m_queue.size()) {
                // Check if removing preloaded track
                if (m_preloadTrack && m_queue[index]->id() == m_preloadTrack->id()) {
                    m_preloadTrack.reset();
                    m_preloadReady = false;
                    m_preloadBuffer.clear();
                    m_preloadStream = 0;
                    QMetaObject::invokeMethod(m_preloadDownloader, "startDownload",
                                              Qt::QueuedConnection,
                                              Q_ARG(QString, QString()),
                                              Q_ARG(QString, QString()));
                }

                m_queue.removeAt(index);
                if (index < m_currentIndex)
                    m_currentIndex--;
            }
        }
    }

    emit queueChanged();
}

void AudioEngine::moveInQueue(int fromIndex, int toIndex)
{
    if (fromIndex < 0 || fromIndex >= m_queue.size() ||
        toIndex < 0 || toIndex >= m_queue.size() ||
        fromIndex == toIndex)
        return;

    auto track = m_queue[fromIndex];
    m_queue.removeAt(fromIndex);
    m_queue.insert(toIndex, track);

    // Adjust m_currentIndex
    if (fromIndex == m_currentIndex) {
        m_currentIndex = toIndex;
    } else if (fromIndex < m_currentIndex && toIndex >= m_currentIndex) {
        m_currentIndex--;
    } else if (fromIndex > m_currentIndex && toIndex <= m_currentIndex) {
        m_currentIndex++;
    }

    emit queueChanged();
}

void AudioEngine::addToQueue(std::shared_ptr<Track> track, int position)
{
    if (!track)
        return;

    if (position < 0 || position >= m_queue.size()) {
        m_queue.append(track);
    } else {
        m_queue.insert(position, track);
        if (position <= m_currentIndex)
            m_currentIndex++;
    }

    emit queueChanged();
}

void AudioEngine::addToQueue(const QList<std::shared_ptr<Track>>& tracks, int position)
{
    if (tracks.isEmpty())
        return;

    if (position < 0 || position >= m_queue.size()) {
        // Append all tracks to the end
        m_queue.append(tracks);
    } else {
        // Insert all tracks at position
        for (int i = 0; i < tracks.size(); ++i) {
            m_queue.insert(position + i, tracks[i]);
        }
        if (position <= m_currentIndex)
            m_currentIndex += tracks.size();
    }

    emit queueChanged();
}

void AudioEngine::clearQueue()
{
    if (m_queue.isEmpty())
        return;

    stop();  // Clean up BASS streams
    m_queue.clear();
    m_currentIndex = -1;
    m_currentTrack.reset();
    emit queueChanged();
}