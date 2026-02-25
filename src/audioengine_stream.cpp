#include "audioengine.h"
#include "deezerapi.h"
#include "streamdownloader.h"
#include "windowsmediacontrols.h"
#include <QMetaObject>

extern "C" {
#include "bassmix.h"
#include "basswasapi.h"
}

// ── Track Loading ───────────────────────────────────────────────────────

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
    destroyStream();
    m_listenReported = false;
    ++m_waveformGeneration;
    emit waveformReady(QVector<float>());

    m_currentTrack = track;

    // If the next track was preloaded, use the already-decrypted data directly
    if (m_preloadReady && m_preloadTrack && m_preloadTrack->id() == track->id()) {
        emit debugLog("[AudioEngine] Using preloaded data for: " + track->title());
        m_currentStreamFormat = m_preloadFormat;
        m_streamBuffer = m_preloadBuffer;  // Already decrypted -- do NOT decrypt again
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

        // Get source stream info and log it
        BASS_CHANNELINFO sci = {};
        if (BASS_ChannelGetInfo(newStream, &sci)) {
            emit debugLog(QString("[AudioEngine] Preloaded track: %1 Hz, %2 channels, format %3")
                              .arg(sci.freq).arg(sci.chans).arg(sci.flags & BASS_SAMPLE_FLOAT ? "float" : "int"));

            if (!ensureOutputRate(sci.freq)) {
                BASS_StreamFree(newStream);
                setState(Stopped);
                return;
            }
        }

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

            if (!isOutputActive()) {
                startMixerOutput();
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
    // User-uploaded tracks use the token as identifier and MP3_MISC format
    QString streamId = track->isUserUploaded() ? track->trackToken() : track->id();
    QString streamFormat = track->isUserUploaded() ? QStringLiteral("MP3_MISC") : QString();
    m_deezerAPI->getStreamUrl(streamId, track->trackToken(), streamFormat);

    // DON'T preload here - preload happens when we're near the end of current track
    // The near-end sync (setupStreamSyncs) will trigger preloadNextTrack() at the right time
}

void AudioEngine::onStreamUrlReceived(const QString& trackId, const QString& url, const QString& format)
{
    // -- Handle preload URL --
    // User-uploaded tracks use the token as stream identifier
    auto matchStreamId = [](const std::shared_ptr<Track>& t, const QString& id) {
        if (!t) return false;
        return t->isUserUploaded() ? (t->trackToken() == id) : (t->id() == id);
    };
    if (matchStreamId(m_preloadTrack, trackId)) {
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
            m_preloadBuffer.clear();  // Reset buffer for new preload
            QMetaObject::invokeMethod(m_preloadDownloader, "startProgressiveDownload", Qt::QueuedConnection,
                                      Q_ARG(QString, url), Q_ARG(QString, trackId));
        }
        return;
    }

    // -- Normal path for pending track --
    if (!matchStreamId(m_pendingTrack, trackId)) {
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

        // Prepare progressive state
        m_progressiveMode = true;
        m_progressivePlaybackStarted = false;
        m_chunkRemainder.clear();
        m_chunkIndex = 0;
        m_totalBytesReceived = 0;
        m_streamBuffer.clear();
        m_downloadTimer.start();

        m_trackKey = DeezerAPI::computeTrackKey(trackId);
        if (m_trackKey.isEmpty()) {
            emit debugLog("[AudioEngine] WARNING: TRACK_XOR_KEY not set, decryption will be skipped");
        }

        // Chunks are decrypted in onStreamChunkReady and accumulated in m_streamBuffer.
        // After 64KB, a STREAMFILE_NOBUFFER stream is created and playback starts.
        // Subsequent chunks are pushed via pushStreamRead callback.
        m_pushStream = 0;
        m_pushInitialOffset = 0;
        m_lastWaveformUpdateBytes = 0;

        // Start progressive download on worker thread
        emit debugLog("[AudioEngine] Starting progressive download...");
        QMetaObject::invokeMethod(m_streamDownloader, "startProgressiveDownload", Qt::QueuedConnection,
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

// ── Stream Creation & Setup ─────────────────────────────────────────────

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
    // Let BASS auto-detect the format from the audio data
    // The mixer (created with BASS_SAMPLE_FLOAT) will handle the conversion
    QMutexLocker locker(&m_bassMutex);
    HSTREAM stream = BASS_StreamCreateFile(
        TRUE,
        data.constData(),
        0,
        static_cast<QWORD>(data.size()),
        BASS_STREAM_DECODE  // DECODE flag crucial for mixer!
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
    double lengthSeconds = (length != (QWORD)-1) ? BASS_ChannelBytes2Seconds(stream, length) : 0.0;

    // Progressive push streams have a fake large length -- always use track metadata
    if (m_pushStream != 0 || lengthSeconds <= 0.0) {
        if (m_currentTrack && m_currentTrack->duration() > 0) {
            lengthSeconds = m_currentTrack->duration();
            length = BASS_ChannelSeconds2Bytes(stream, lengthSeconds);
        }
    }
    if (lengthSeconds <= 0.0) return;  // Can't set syncs without knowing length

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
        // For push streams, BASS_ChannelGetLength is unreliable (fake large length) -- use metadata
        double duration = 0;
        if (m_pushStream != 0 && m_currentTrack && m_currentTrack->duration() > 0) {
            duration = m_currentTrack->duration();
        } else {
            duration = BASS_ChannelBytes2Seconds(stream, BASS_ChannelGetLength(stream, BASS_POS_BYTE));
        }
        if (duration > 0 && m_streamBuffer.size() > 0)
            bitrate = static_cast<int>((static_cast<double>(m_streamBuffer.size()) * 8.0) / (duration * 1000.0));
        QString chanStr = ci.chans == 1 ? "mono" : ci.chans == 2 ? "stereo" : QString("%1ch").arg(ci.chans);
        QString fmt = m_currentStreamFormat.isEmpty() ? "unknown" : m_currentStreamFormat;
        QString info = QString("%1 | %2 kbps | %3 Hz | %4").arg(fmt).arg(bitrate).arg(ci.freq).arg(chanStr);
        emit streamInfoChanged(info);
        emit debugLog(QString("[AudioEngine] Stream: %1").arg(info));
    }
}

// ── Stream Destruction ──────────────────────────────────────────────────

void AudioEngine::destroyStream()
{
    // Signal the blocking read callback to stop BEFORE locking m_bassMutex.
    // pushStreamRead blocks on m_bufferMutex (not m_bassMutex), so setting
    // this atomic flag lets it return EOF without touching m_bassMutex.
    // This prevents deadlock: destroyStream holds m_bassMutex -> BASS_ChannelStop
    // waits for mixer thread -> mixer thread in pushStreamRead checks atomic -> exits.
    m_progressiveMode.store(false);

    QMutexLocker locker(&m_bassMutex);

    // Remove queue sync BEFORE stopping, so dequeuing sources won't fire stale callbacks
    if (m_queueSync && m_mixerStream) {
        BASS_ChannelRemoveSync(m_mixerStream, m_queueSync);
        m_queueSync = 0;
    }

    // Bump generation so any already-queued callbacks are ignored
    m_dequeueGeneration++;

    // Stop output FIRST so BASS is no longer decoding from any source
    if (m_outputMode != OutputDirectSound) {
        BASS_WASAPI_Stop(TRUE);
        // Do NOT BASS_ChannelStop the decode mixer -- it can't be restarted.
        // WASAPI_Stop already prevents pulling data from the mixer.
    } else if (m_mixerStream) {
        BASS_ChannelStop(m_mixerStream);
    }

    // Now safely remove and free all source streams.
    // Stopping alone does NOT remove sources -- they stay attached and
    // would play again when the mixer is restarted.
    if (m_currentStream) {
        BASS_Mixer_ChannelRemove(m_currentStream);
        BASS_StreamFree(m_currentStream);
    }
    if (m_preloadStream) {
        BASS_Mixer_ChannelRemove(m_preloadStream);
        BASS_StreamFree(m_preloadStream);
    }
    // Free push stream if it's separate from currentStream (e.g. failed to start playback)
    if (m_pushStream && m_pushStream != m_currentStream) {
        BASS_StreamFree(m_pushStream);
    }
    m_currentEndSync = 0;
    m_currentNearEndSync = 0;

    // Clear references
    m_currentStream = 0;
    m_preloadStream = 0;
    m_pushStream = 0;
    {
        QMutexLocker bufLock(&m_bufferMutex);
        m_streamBuffer.clear();
    }

    // Restore QUEUE mode if it was disabled for progressive streaming
    if (m_mixerStream) {
        BASS_ChannelFlags(m_mixerStream, BASS_MIXER_QUEUE, BASS_MIXER_QUEUE);
    }

    // Reset progressive streaming state (m_progressiveMode already set false above)
    m_progressivePlaybackStarted = false;
    m_chunkRemainder.clear();
    m_chunkIndex = 0;
    m_pushInitialOffset = 0;
    m_lastWaveformUpdateBytes = 0;
    m_trackKey.clear();
    m_totalBytesReceived = 0;

    // Cancel worker downloads (empty URL aborts any in-progress download)
    QMetaObject::invokeMethod(m_streamDownloader, "startProgressiveDownload", Qt::QueuedConnection,
                              Q_ARG(QString, QString()), Q_ARG(QString, QString()));
    QMetaObject::invokeMethod(m_preloadDownloader, "startProgressiveDownload", Qt::QueuedConnection,
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

// ── Gapless Track Transitions ───────────────────────────────────────────

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
    // If it matches m_currentStream, this is the initial activation -- ignore it.
    if (streamHandle == m_currentStream) {
        emit debugLog(QString("[AudioEngine] Stream %1 is current stream (initial activation, ignoring)")
                     .arg(streamHandle));
        return;
    }

    // A different stream started playing -- this is a gapless track transition.
    HSTREAM oldStream = m_currentStream;
    emit debugLog(QString("[AudioEngine] Track transition: stream %1 -> %2, advancing queue from index %3 to %4")
                 .arg(oldStream).arg(streamHandle)
                 .arg(m_currentIndex).arg(m_currentIndex + 1));

    m_currentStream = streamHandle;
    m_preloadStream = 0;

    if (m_repeatMode != RepeatOne) {
        m_currentIndex++;

        // Wrap around for RepeatAll mode
        if (m_currentIndex >= m_queue.size() && m_repeatMode == RepeatAll) {
            m_currentIndex = 0;
            emit debugLog("[AudioEngine] Wrapped m_currentIndex to 0 (RepeatAll)");
        }
    } else {
        emit debugLog("[AudioEngine] RepeatOne: keeping m_currentIndex at " + QString::number(m_currentIndex));
    }

    // Free the finished stream BEFORE overwriting its backing buffer.
    // The old stream still references m_streamBuffer's raw memory
    // (BASS_StreamCreateFile with mem=TRUE). Must free it first.
    if (oldStream) {
        BASS_Mixer_ChannelRemove(oldStream);
        BASS_StreamFree(oldStream);
    }

    // Now safe to replace the buffer -- no BASS stream references the old data.
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

        emit debugLog(QString("[AudioEngine] trackChanged signal emitted"));

        // Preload next track
        preloadNextTrack();
    } else {
        emit debugLog("[AudioEngine] Reached end of queue");
        m_currentTrack.reset();
        emit trackChanged(nullptr);
    }
}
