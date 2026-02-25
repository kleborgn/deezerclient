#include "audioengine.h"
#include "deezerapi.h"
#include "streamdownloader.h"
#include "windowsmediacontrols.h"
#include <QTimer>
#include <QThread>
#include <QMetaObject>

extern "C" {
#include "bassmix.h"
#include "basswasapi.h"
}

// ── Constructor & Destructor ────────────────────────────────────────────

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
    , m_preloadDownloader(nullptr)
    , m_deezerAPI(nullptr)
    , m_currentIndex(-1)
    , m_repeatMode(RepeatOff)
    , m_preloadStream(0)
    , m_outputMode(OutputDirectSound)
    , m_wasapiDevice(-1)
    , m_outputSampleRate(44100)
{
    qRegisterMetaType<DWORD>("DWORD");

    m_positionTimer = new QTimer(this);
    connect(m_positionTimer, &QTimer::timeout, this, &AudioEngine::updatePosition);

    m_spectrumTimer = new QTimer(this);
    m_spectrumTimer->setInterval(33); // ~30 FPS
    m_spectrumEnabled = true;  // Enabled by default for visualizer support
    connect(m_spectrumTimer, &QTimer::timeout, this, &AudioEngine::updateSpectrum);

    m_downloadThread = new QThread(this);
    m_streamDownloader = new StreamDownloader();
    m_streamDownloader->moveToThread(m_downloadThread);
    connect(m_streamDownloader, &StreamDownloader::chunkReady, this, &AudioEngine::onStreamChunkReady, Qt::QueuedConnection);
    connect(m_streamDownloader, &StreamDownloader::progressiveDownloadFinished, this, &AudioEngine::onProgressiveDownloadFinished, Qt::QueuedConnection);

    m_preloadDownloader = new StreamDownloader();
    m_preloadDownloader->moveToThread(m_downloadThread);
    connect(m_preloadDownloader, &StreamDownloader::chunkReady, this, &AudioEngine::onPreloadChunkReady, Qt::QueuedConnection);
    connect(m_preloadDownloader, &StreamDownloader::progressiveDownloadFinished, this, &AudioEngine::onPreloadDownloadFinished, Qt::QueuedConnection);

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

// ── Initialization & Shutdown ───────────────────────────────────────────

bool AudioEngine::initialize()
{
    if (m_initialized) {
        return true;
    }

    emit debugLog("[AudioEngine] initialize() called");

    DWORD mixerRate = 44100;  // Start with standard rate, will switch to track rate on first load
    DWORD mixerExtraFlags = 0;
    DWORD mixerFormat = BASS_SAMPLE_FLOAT;  // Default to float, but will adjust based on device support

    if (m_outputMode == OutputDirectSound) {
        // DirectSound: default device, 44100 Hz
        if (!BASS_Init(-1, 44100, 0, nullptr, nullptr)) {
            emit error("Failed to initialize BASS audio library");
            return false;
        }
        emit debugLog("[AudioEngine] BASS initialized (DirectSound)");
    } else {
        // WASAPI Shared or Exclusive: use "no sound" BASS device
        if (!BASS_Init(0, 44100, 0, nullptr, nullptr)) {
            emit error("Failed to initialize BASS (no-sound device)");
            return false;
        }

        // Find WASAPI device index
        int wasapiDev = (m_wasapiDevice >= 0) ? m_wasapiDevice : -1;
        if (wasapiDev < 0) {
            // Find default output device
            BASS_WASAPI_DEVICEINFO devInfo;
            for (DWORD d = 0; BASS_WASAPI_GetDeviceInfo(d, &devInfo); d++) {
                if ((devInfo.flags & BASS_DEVICE_ENABLED) &&
                    (devInfo.flags & BASS_DEVICE_DEFAULT) &&
                    !(devInfo.flags & BASS_DEVICE_INPUT) &&
                    !(devInfo.flags & BASS_DEVICE_LOOPBACK)) {
                    wasapiDev = static_cast<int>(d);
                    break;
                }
            }
        }

        // Mixer must be a decode channel for WASAPI to pull from
        mixerExtraFlags = BASS_STREAM_DECODE;
        // Mixer format will be determined after probing the WASAPI device
        mixerFormat = 0;

        // For exclusive mode, use the device's native sample rate
        // (exclusive mode requires an exact format match -- 44100 will fail on 48000-native devices)
        if (m_outputMode == OutputWasapiExclusive) {
            BASS_WASAPI_DEVICEINFO devInfo;
            if (BASS_WASAPI_GetDeviceInfo(wasapiDev, &devInfo) && devInfo.mixfreq > 0) {
                mixerRate = devInfo.mixfreq;
                emit debugLog(QString("[AudioEngine] WASAPI device native rate: %1 Hz").arg(devInfo.mixfreq));
            }
        }

        emit debugLog(QString("[AudioEngine] BASS initialized (no-sound), WASAPI device %1, initial rate %2 Hz")
                      .arg(wasapiDev).arg(mixerRate));

        m_wasapiDevice = wasapiDev;
    }

    m_outputSampleRate = mixerRate;

    // For WASAPI modes, probe the device to discover the negotiated format and sample rate
    if (m_outputMode != OutputDirectSound) {
        DWORD wasapiFlags = 0;
        if (m_outputMode == OutputWasapiExclusive) {
            wasapiFlags = BASS_WASAPI_EXCLUSIVE;
        }

        // Check if exclusive mode is supported before trying to init
        if (m_outputMode == OutputWasapiExclusive) {
            DWORD checkResult = BASS_WASAPI_CheckFormat(m_wasapiDevice, mixerRate, 2,
                                                        BASS_WASAPI_EXCLUSIVE);
            if (checkResult == (DWORD)-1) {
                int checkErr = BASS_ErrorGetCode();
                emit debugLog(QString("[AudioEngine] WASAPI Exclusive not supported at %1 Hz (error %2) "
                                      "-- another application may be using the device")
                              .arg(mixerRate).arg(checkErr));
            } else {
                QString fmtName;
                switch (checkResult) {
                    case BASS_WASAPI_FORMAT_FLOAT: fmtName = "32-bit float"; break;
                    case BASS_WASAPI_FORMAT_8BIT:  fmtName = "8-bit"; break;
                    case BASS_WASAPI_FORMAT_16BIT: fmtName = "16-bit"; break;
                    case BASS_WASAPI_FORMAT_24BIT: fmtName = "24-bit"; break;
                    case BASS_WASAPI_FORMAT_32BIT: fmtName = "32-bit int"; break;
                    default: fmtName = QString("format %1").arg(checkResult); break;
                }
                emit debugLog(QString("[AudioEngine] WASAPI Exclusive supported: %1 Hz, %2")
                              .arg(mixerRate).arg(fmtName));
            }
        }

        // Try to initialize WASAPI with requested mode
        if (!BASS_WASAPI_Init(m_wasapiDevice, mixerRate, 2, wasapiFlags, 0, 0, nullptr, nullptr)) {
            int err = BASS_ErrorGetCode();
            if (err == 37 && m_outputMode == OutputWasapiExclusive) {
                emit debugLog(QString("[AudioEngine] WASAPI Exclusive not available (error %1), falling back to Shared")
                              .arg(err));
                m_outputMode = OutputWasapiShared;
                wasapiFlags = 0;
                if (!BASS_WASAPI_Init(m_wasapiDevice, mixerRate, 2, wasapiFlags, 0, 0, nullptr, nullptr)) {
                    int err2 = BASS_ErrorGetCode();
                    emit error(QString("Failed to initialize WASAPI Shared mode: error %1").arg(err2));
                    BASS_Free();
                    return false;
                }
                emit debugLog("[AudioEngine] WASAPI Shared mode initialized successfully");
            } else {
                emit error(QString("Failed to initialize WASAPI: error %1").arg(err));
                BASS_Free();
                return false;
            }
        }

        // Get WASAPI negotiated format for logging
        BASS_WASAPI_INFO wasapiInfo;
        if (BASS_WASAPI_GetInfo(&wasapiInfo)) {
            QString formatStr;
            switch (wasapiInfo.format) {
                case BASS_WASAPI_FORMAT_FLOAT: formatStr = "32-bit float"; break;
                case BASS_WASAPI_FORMAT_8BIT:  formatStr = "8-bit"; break;
                case BASS_WASAPI_FORMAT_16BIT: formatStr = "16-bit"; break;
                case BASS_WASAPI_FORMAT_24BIT: formatStr = "24-bit"; break;
                case BASS_WASAPI_FORMAT_32BIT: formatStr = "32-bit int"; break;
                default: formatStr = QString("format %1").arg(wasapiInfo.format); break;
            }
        emit debugLog(QString("[AudioEngine] WASAPI negotiated: %1 Hz, %2 ch, %3")
                  .arg(wasapiInfo.freq).arg(wasapiInfo.chans).arg(formatStr));

        // Update mixer rate to negotiated rate
        mixerRate = wasapiInfo.freq;
        m_outputSampleRate = mixerRate;

        // BASS mixer only supports 16-bit int or 32-bit float internally.
        // For 24-bit and 32-bit WASAPI devices, use float -- BASS handles
        // float->24/32-bit conversion via WASAPIPROC_BASS.
        // Only use 16-bit mixer for 8-bit and 16-bit devices.
        switch (wasapiInfo.format) {
            case BASS_WASAPI_FORMAT_8BIT:
            case BASS_WASAPI_FORMAT_16BIT:
                mixerFormat = 0;
                break;
            default:
                mixerFormat = BASS_SAMPLE_FLOAT;
                break;
        }
        }

        emit debugLog(QString("[AudioEngine] Creating WASAPI mixer at %1 Hz, format %2")
                      .arg(mixerRate).arg(mixerFormat == BASS_SAMPLE_FLOAT ? "float" : "int"));

        // Stop WASAPI so we can reinit it with mixer callback
        BASS_WASAPI_Stop(TRUE);
        BASS_WASAPI_Free();
    }

    // Create mixer stream with correct format
    // The mixer's format will drive WASAPI's format negotiation
    m_mixerStream = BASS_Mixer_StreamCreate(
        mixerRate,
        2,
        mixerFormat | BASS_MIXER_QUEUE | BASS_MIXER_RESUME | mixerExtraFlags
    );

    if (!m_mixerStream) {
        int err = BASS_ErrorGetCode();
        emit error(QString("Failed to create mixer stream: error %1").arg(err));
        BASS_Free();
        return false;
    }

        // Initialize WASAPI output if needed
        if (m_outputMode != OutputDirectSound) {
            DWORD wasapiFlags = BASS_WASAPI_BUFFER;  // Enable double-buffering for BASS_WASAPI_GetData
            if (m_outputMode == OutputWasapiExclusive) {
                wasapiFlags |= BASS_WASAPI_EXCLUSIVE;
            }

            BOOL wasapiOk = BASS_WASAPI_Init(
                m_wasapiDevice, mixerRate, 2, wasapiFlags,
                0, 0,
                WASAPIPROC_BASS, reinterpret_cast<void*>(m_mixerStream)
            );

            if (!wasapiOk) {
                int err = BASS_ErrorGetCode();
                emit error(QString("Failed to initialize WASAPI: error %1").arg(err));
                BASS_StreamFree(m_mixerStream);
                m_mixerStream = 0;
                BASS_Free();
                return false;
            }

            // Get WASAPI info for logging and verification
            BASS_WASAPI_INFO wasapiInfo;
            if (BASS_WASAPI_GetInfo(&wasapiInfo)) {
                QString formatStr;
                switch (wasapiInfo.format) {
                    case BASS_WASAPI_FORMAT_FLOAT: formatStr = "32-bit float"; break;
                    case BASS_WASAPI_FORMAT_8BIT:  formatStr = "8-bit"; break;
                    case BASS_WASAPI_FORMAT_16BIT: formatStr = "16-bit"; break;
                    case BASS_WASAPI_FORMAT_24BIT: formatStr = "24-bit"; break;
                    case BASS_WASAPI_FORMAT_32BIT: formatStr = "32-bit int"; break;
                    default: formatStr = QString("format %1").arg(wasapiInfo.format); break;
                }
                emit debugLog(QString("[AudioEngine] WASAPI initialized: %1 Hz, %2 ch, %3")
                          .arg(wasapiInfo.freq).arg(wasapiInfo.chans).arg(formatStr));

                // In exclusive mode, verify sample rate matches
                if (m_outputMode == OutputWasapiExclusive && wasapiInfo.freq != mixerRate) {
                    emit debugLog(QString("[AudioEngine] WARNING: WASAPI exclusive mode negotiated %1 Hz, mixer is %2 Hz - potential speed issue!")
                                  .arg(wasapiInfo.freq).arg(mixerRate));
                    m_outputSampleRate = wasapiInfo.freq;  // Update to actual negotiated rate
                }
            }

            QString modeStr = (m_outputMode == OutputWasapiExclusive) ? "Exclusive" : "Shared";
            emit debugLog(QString("[AudioEngine] WASAPI %1 mode active at %2 Hz").arg(modeStr).arg(m_outputSampleRate));
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
        emit debugLog(QString("[AudioEngine] Failed to set BASS_SYNC_MIXER_QUEUE: error %1").arg(err));
    } else {
        emit debugLog("[AudioEngine] BASS_SYNC_MIXER_QUEUE sync set");
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

    // Free WASAPI before BASS
    if (m_outputMode != OutputDirectSound) {
        BASS_WASAPI_Free();
    }

    BASS_Free();
    m_initialized = false;
}

// ── Playback Control ────────────────────────────────────────────────────

void AudioEngine::play()
{
    if (!m_initialized || !m_mixerStream) {
        emit debugLog("[AudioEngine] play() ignored: not initialized or no mixer");
        return;
    }

    QMutexLocker locker(&m_bassMutex);

    if (startMixerOutput()) {
        setState(Playing);
        m_positionTimer->start(100);
        emit debugLog("[AudioEngine] Playing mixer stream");
    } else {
        emit debugLog(QString("[AudioEngine] startMixerOutput() failed: %1").arg(BASS_ErrorGetCode()));
    }
}

void AudioEngine::pause()
{
    if (!m_initialized || !m_mixerStream) {
        return;
    }

    QMutexLocker locker(&m_bassMutex);
    if (m_outputMode != OutputDirectSound) {
        BASS_WASAPI_Stop(FALSE);  // FALSE = don't reset, just pause
    } else {
        BASS_ChannelPause(m_mixerStream);
    }
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

    // Stop output but DON'T remove the stream
    // This allows resuming from the same position
    if (m_outputMode != OutputDirectSound) {
        BASS_WASAPI_Stop(TRUE);  // TRUE = reset
    } else {
        BASS_ChannelStop(m_mixerStream);
    }
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

    // During progressive download, use track metadata for seek calculation
    QWORD length = BASS_ChannelGetLength(m_currentStream, BASS_POS_BYTE);
    if (length == (QWORD)-1 || m_pushStream != 0) {
        if (m_currentTrack && m_currentTrack->duration() > 0) {
            double targetSeconds = position * m_currentTrack->duration();
            QWORD seekPos = BASS_ChannelSeconds2Bytes(m_currentStream, targetSeconds);
            BASS_Mixer_ChannelSetPosition(m_currentStream, seekPos,
                                          BASS_POS_BYTE | BASS_POS_MIXER_RESET);
            return;
        }
        emit debugLog("[AudioEngine] Cannot seek: stream length unknown");
        return;
    }

    QWORD seekPos = static_cast<QWORD>(length * position);

    BASS_Mixer_ChannelSetPosition(m_currentStream, seekPos,
                                   BASS_POS_BYTE | BASS_POS_MIXER_RESET);
}

void AudioEngine::setDeezerAPI(DeezerAPI* api)
{
    m_deezerAPI = api;
}

// ── Volume & Repeat ─────────────────────────────────────────────────────

void AudioEngine::setRepeatMode(RepeatMode mode)
{
    if (m_repeatMode != mode) {
        RepeatMode oldMode = m_repeatMode;
        m_repeatMode = mode;
        emit repeatModeChanged(m_repeatMode);

        // When switching away from RepeatOne, the preloaded stream may be
        // a duplicate of the current track (queued for looping). Remove it
        // and preload the correct next track instead.
        if (oldMode == RepeatOne && m_currentStream) {
            if (m_preloadStream) {
                BASS_Mixer_ChannelRemove(m_preloadStream);
                BASS_StreamFree(m_preloadStream);
                m_preloadStream = 0;
                emit debugLog("[AudioEngine] Cleared RepeatOne preloaded stream");
            }
            m_preloadTrack.reset();
            m_preloadBuffer.clear();
            m_preloadReady = false;

            // Preload the correct next track for the new mode
            preloadNextTrack();
        }
    }
}

void AudioEngine::setVolume(float volume)
{
    m_volume = qBound(0.0f, volume, 1.0f);

    if (m_initialized && m_mixerStream) {
        BASS_ChannelSetAttribute(m_mixerStream, BASS_ATTRIB_VOL, m_volume);
    }
}

// ── State Management ────────────────────────────────────────────────────

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

// ── BASS Sync Callbacks ─────────────────────────────────────────────────

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
