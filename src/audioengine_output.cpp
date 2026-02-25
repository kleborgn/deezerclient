#include "audioengine.h"
#include <QThread>

extern "C" {
#include "bassmix.h"
#include "basswasapi.h"
}

// ── Output Mode Helpers ─────────────────────────────────────────────────

bool AudioEngine::startMixerOutput()
{
    if (m_outputMode != OutputDirectSound) {
        return BASS_WASAPI_Start();
    }
    return BASS_ChannelPlay(m_mixerStream, FALSE);
}

bool AudioEngine::isOutputActive()
{
    if (m_outputMode != OutputDirectSound) {
        return BASS_WASAPI_IsStarted();
    }
    return BASS_ChannelIsActive(m_mixerStream) == BASS_ACTIVE_PLAYING;
}

void AudioEngine::stopMixerOutput()
{
    if (m_outputMode != OutputDirectSound) {
        BASS_WASAPI_Stop(TRUE);
        // Do NOT BASS_ChannelStop the decode mixer -- it can't be restarted.
    } else {
        BASS_ChannelStop(m_mixerStream);
    }
}

bool AudioEngine::ensureOutputRate(DWORD sourceFreq)
{
    // Only matters for exclusive mode -- shared mode resamples via Windows mixer,
    // DirectSound resamples via BASS.
    if (m_outputMode != OutputWasapiExclusive)
        return true;

    // ALWAYS verify the actual WASAPI rate, even if it looks like it matches
    // The device might be running at a different rate than m_outputSampleRate

    // Get the ACTUAL WASAPI configuration
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

        emit debugLog(QString("[AudioEngine] WASAPI actual config: %1 Hz, %2 ch, %3 (requested: %4 Hz)")
                      .arg(wasapiInfo.freq).arg(wasapiInfo.chans).arg(formatStr).arg(sourceFreq));

        // If the actual WASAPI rate differs from either source or current, we need to adjust
        if (wasapiInfo.freq != sourceFreq || wasapiInfo.freq != m_outputSampleRate) {
            emit debugLog(QString("[AudioEngine] RATE MISMATCH! Device at %1 Hz, source %2 Hz, mixer %3 Hz")
                          .arg(wasapiInfo.freq).arg(sourceFreq).arg(m_outputSampleRate));
            // Fall through to reinitialize
        } else {
            // Everything matches - no changes needed
            return true;
        }
    } else {
        emit debugLog("[AudioEngine] WARNING: Could not get WASAPI info");
    }

    if (sourceFreq == m_outputSampleRate && wasapiInfo.freq == sourceFreq)
        return true;

    emit debugLog(QString("[AudioEngine] Exclusive mode: switching output rate %1 -> %2 Hz")
                  .arg(m_outputSampleRate).arg(sourceFreq));

    // Stop WASAPI output
    BASS_WASAPI_Stop(TRUE);
    BASS_WASAPI_Free();

    // Free old mixer (sources were already removed/freed by caller)
    if (m_mixerStream) {
        if (m_queueSync) {
            BASS_ChannelRemoveSync(m_mixerStream, m_queueSync);
            m_queueSync = 0;
        }
        BASS_StreamFree(m_mixerStream);
        m_mixerStream = 0;
    }

    DWORD wasapiFlags = BASS_WASAPI_EXCLUSIVE;

    // Initialize WASAPI first to see what format it actually negotiates
    // This allows us to create a mixer with the exact same sample rate and format
    // to avoid any sample rate conversion that could cause speed issues
    if (!BASS_WASAPI_Init(m_wasapiDevice, sourceFreq, 2, wasapiFlags, 0, 0, nullptr, nullptr)) {
        int err = BASS_ErrorGetCode();
        emit error(QString("Failed to init WASAPI at %1 Hz: error %2").arg(sourceFreq).arg(err));
        return false;
    }

    // Get the negotiated WASAPI format and sample rate
    if (!BASS_WASAPI_GetInfo(&wasapiInfo)) {
        int err = BASS_ErrorGetCode();
        emit error(QString("Failed to get WASAPI info: error %1").arg(err));
        BASS_WASAPI_Free();
        return false;
    }

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

    // CRITICAL: If WASAPI negotiated a different sample rate than requested,
    // we must use the negotiated rate to avoid playback speed issues
    if (wasapiInfo.freq != sourceFreq) {
        emit debugLog(QString("[AudioEngine] WARNING: WASAPI negotiated %1 Hz instead of requested %2 Hz")
                      .arg(wasapiInfo.freq).arg(sourceFreq));
        sourceFreq = wasapiInfo.freq;  // Use the actual rate WASAPI wants
    }

    // Stop WASAPI to recreate mixer with correct sample rate
    BASS_WASAPI_Stop(TRUE);
    BASS_WASAPI_Free();

    // Create mixer at the WASAPI-negotiated sample rate and format
    // BASS mixer only supports 16-bit int or 32-bit float -- use float for
    // 24/32-bit devices so WASAPIPROC_BASS can convert to the native format.
    DWORD mixerFormatFlag;
    switch (wasapiInfo.format) {
        case BASS_WASAPI_FORMAT_8BIT:
        case BASS_WASAPI_FORMAT_16BIT:
            mixerFormatFlag = 0;
            break;
        default:
            mixerFormatFlag = BASS_SAMPLE_FLOAT;
            break;
    }

    emit debugLog(QString("[AudioEngine] Creating mixer with %1 Hz, format flag %2")
                  .arg(sourceFreq).arg(mixerFormatFlag == BASS_SAMPLE_FLOAT ? "float" : "int"));

    m_mixerStream = BASS_Mixer_StreamCreate(
        sourceFreq, 2,
        mixerFormatFlag | BASS_MIXER_QUEUE | BASS_MIXER_RESUME | BASS_STREAM_DECODE
    );

    if (!m_mixerStream) {
        int err = BASS_ErrorGetCode();
        emit error(QString("Failed to create mixer at %1 Hz: error %2").arg(sourceFreq).arg(err));
        return false;
    }

    // Initialize WASAPI again with the mixer callback
    // Use the negotiated sample rate to ensure speed is correct
    // BASS_WASAPI_BUFFER enables double-buffering for BASS_WASAPI_GetData (spectrum/PCM visualization)
    BOOL ok = BASS_WASAPI_Init(
        m_wasapiDevice, sourceFreq, 2, wasapiFlags | BASS_WASAPI_BUFFER,
        0, 0,
        WASAPIPROC_BASS, reinterpret_cast<void*>(m_mixerStream)
    );

    if (!ok) {
        int err = BASS_ErrorGetCode();
        emit error(QString("Failed to init WASAPI at %1 Hz (with mixer): error %2").arg(sourceFreq).arg(err));
        BASS_StreamFree(m_mixerStream);
        m_mixerStream = 0;
        return false;
    }

    // Verify final WASAPI configuration
    if (BASS_WASAPI_GetInfo(&wasapiInfo)) {
        QString finalFormatStr;
        switch (wasapiInfo.format) {
            case BASS_WASAPI_FORMAT_FLOAT: finalFormatStr = "32-bit float"; break;
            case BASS_WASAPI_FORMAT_8BIT:  finalFormatStr = "8-bit"; break;
            case BASS_WASAPI_FORMAT_16BIT: finalFormatStr = "16-bit"; break;
            case BASS_WASAPI_FORMAT_24BIT: finalFormatStr = "24-bit"; break;
            case BASS_WASAPI_FORMAT_32BIT: finalFormatStr = "32-bit int"; break;
            default: finalFormatStr = QString("format %1").arg(wasapiInfo.format); break;
        }
        emit debugLog(QString("[AudioEngine] WASAPI final config: %1 Hz, %2 ch, %3")
                      .arg(wasapiInfo.freq).arg(wasapiInfo.chans).arg(finalFormatStr));
    }

    // Update output sample rate to what WASAPI actually negotiated
    m_outputSampleRate = sourceFreq;
    BASS_ChannelSetAttribute(m_mixerStream, BASS_ATTRIB_VOL, m_volume);

    // Re-create queue sync on new mixer
    m_queueSync = BASS_ChannelSetSync(
        m_mixerStream, BASS_SYNC_MIXER_QUEUE, 0, queueSyncCallback, this
    );

    emit debugLog(QString("[AudioEngine] WASAPI Exclusive: mixer at %1 Hz, source will be resampled to match")
                  .arg(sourceFreq));
    return true;
}

void AudioEngine::setOutputMode(OutputMode mode, int wasapiDevice)
{
    m_outputMode = mode;
    m_wasapiDevice = wasapiDevice;
}

bool AudioEngine::reinitialize(OutputMode mode, int wasapiDevice)
{
    // Save current state
    auto savedQueue = m_queue;
    int savedIndex = m_currentIndex;
    auto savedTrack = m_currentTrack;
    float savedVolume = m_volume;
    bool savedGapless = m_gaplessEnabled;
    RepeatMode savedRepeat = m_repeatMode;
    QString savedContextType = m_contextType;
    QString savedContextId = m_contextId;
    double savedPosition = position();
    PlaybackState savedState = m_state;

    emit debugLog(QString("[AudioEngine] reinitialize: %1 -> %2 (device %3)")
                  .arg(m_outputMode).arg(mode).arg(wasapiDevice));

    // Full teardown
    shutdown();

    // Set new mode
    m_outputMode = mode;
    m_wasapiDevice = wasapiDevice;

    // Re-initialize
    if (!initialize()) {
        emit error("Failed to reinitialize audio engine");
        return false;
    }

    // Restart download thread (shutdown() killed it)
    if (m_downloadThread && !m_downloadThread->isRunning()) {
        m_downloadThread->start();
    }

    // Restore state
    m_volume = savedVolume;
    BASS_ChannelSetAttribute(m_mixerStream, BASS_ATTRIB_VOL, m_volume);
    m_gaplessEnabled = savedGapless;
    m_repeatMode = savedRepeat;
    m_queue = savedQueue;
    m_currentIndex = savedIndex;
    m_contextType = savedContextType;
    m_contextId = savedContextId;

    // Reload track if one was playing
    if (savedTrack && savedIndex >= 0 && savedIndex < m_queue.size()) {
        loadTrack(savedTrack);
    }

    return true;
}

QList<AudioEngine::AudioDevice> AudioEngine::enumerateWasapiDevices()
{
    QList<AudioDevice> devices;
    BASS_WASAPI_DEVICEINFO info;

    for (DWORD i = 0; BASS_WASAPI_GetDeviceInfo(i, &info); i++) {
        // Skip input, loopback, and disabled devices
        if (info.flags & BASS_DEVICE_INPUT) continue;
        if (info.flags & BASS_DEVICE_LOOPBACK) continue;
        if (!(info.flags & BASS_DEVICE_ENABLED)) continue;

        AudioDevice dev;
        dev.index = static_cast<int>(i);
        dev.name = QString::fromLocal8Bit(info.name);
        dev.mixfreq = info.mixfreq;
        dev.mixchans = info.mixchans;
        dev.type = info.type;
        dev.isDefault = (info.flags & BASS_DEVICE_DEFAULT) != 0;
        devices.append(dev);
    }

    return devices;
}
