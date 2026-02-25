#include "audioengine.h"
#include "deezerapi.h"
#include "blowfish_jukebox.h"
#include "windowsmediacontrols.h"
#include <QThread>
#include <QTimer>
#include <QtConcurrent>
#include <QFutureWatcher>
#include <QMetaObject>

extern "C" {
#include "bassmix.h"
#include "basswasapi.h"
}

// Forward declaration (defined in audioengine_visualization.cpp)
// Needed for progressive waveform updates during download
QVector<float> computeWaveformFromBuffer(const QByteArray& data, int numPeaks,
                                         std::atomic<int>* generationPtr, int currentGeneration,
                                         QRecursiveMutex* bassMutex,
                                         double completionRatio = 1.0);

// ── BASS FILEPROCS for STREAMFILE_NOBUFFER (progressive streaming) ──
// NOBUFFER calls pushStreamRead on the calling thread:
//   - During creation (main thread): serves buffered data, returns 0 when empty
//   - During playback (mixer thread): serves data, blocks when empty (sleep loop)
// This avoids STREAMFILE_BUFFER's internal thread which bypasses our thread check.

void CALLBACK AudioEngine::pushStreamClose(void* user) { Q_UNUSED(user); }

QWORD CALLBACK AudioEngine::pushStreamLength(void* user) {
    AudioEngine* self = static_cast<AudioEngine*>(user);
    // During progressive download, return a large fake length so BASS doesn't
    // treat the file as empty/complete and end playback prematurely.
    // Returning 0 means "0 bytes" to BASS (not "unknown"), causing immediate EOF.
    // pushStreamRead returning 0 is the actual EOF signal.
    // After download completes, return the real file size.
    if (self->m_progressiveMode.load())
        return 0xFFFFFFFF;  // ~4GB -- BASS won't reach this before real EOF
    QMutexLocker locker(&self->m_bufferMutex);
    return static_cast<QWORD>(self->m_streamBuffer.size());
}

DWORD CALLBACK AudioEngine::pushStreamRead(void* buffer, DWORD length, void* user) {
    AudioEngine* self = static_cast<AudioEngine*>(user);

    while (true) {
        {
            QMutexLocker locker(&self->m_bufferMutex);
            int available = self->m_streamBuffer.size() - self->m_pushInitialOffset;
            if (available > 0) {
                DWORD toRead = qMin(static_cast<DWORD>(available), length);
                memcpy(buffer, self->m_streamBuffer.constData() + self->m_pushInitialOffset, toRead);
                self->m_pushInitialOffset += toRead;
                return toRead;
            }
        }

        // No data available
        if (!self->m_progressiveMode.load()) {
            return 0;  // Download finished or cancelled -- EOF
        }

        // On main thread (during stream creation): don't block, return what we have
        if (QThread::currentThread() == self->thread()) {
            return 0;
        }

        // On mixer thread: wait for more data to arrive
        QThread::msleep(5);
    }
}

BOOL CALLBACK AudioEngine::pushStreamSeek(QWORD offset, void* user) {
    AudioEngine* self = static_cast<AudioEngine*>(user);
    // Allow seeks within the buffered data on any thread.
    // BASS may seek during creation (format detection) and during decoding
    // (e.g. MP3 bit reservoir). Since we keep all data in m_streamBuffer,
    // seeking within it is safe.
    QMutexLocker locker(&self->m_bufferMutex);
    if (offset <= static_cast<QWORD>(self->m_streamBuffer.size())) {
        self->m_pushInitialOffset = static_cast<int>(offset);
        return TRUE;
    }
    return FALSE;
}

// ── Progressive streaming handlers ──

void AudioEngine::onStreamChunkReady(const QByteArray& chunk, const QString& trackId)
{
    if (!m_currentTrack || m_currentTrack->id() != trackId || !m_progressiveMode)
        return;

    // Prepend leftover bytes from previous chunk
    QByteArray workBuffer = m_chunkRemainder + chunk;
    m_chunkRemainder.clear();

    static const quint8 IV[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    static const int BLOCK_SIZE = 2048;

    int offset = 0;
    QByteArray decryptedBatch;
    decryptedBatch.reserve(workBuffer.size());

    while (offset + BLOCK_SIZE <= workBuffer.size()) {
        if (m_chunkIndex % 3 == 0 && !m_trackKey.isEmpty()) {
            blowfishCbcDecryptChunk(
                reinterpret_cast<const quint8*>(m_trackKey.constData()),
                IV,
                reinterpret_cast<quint8*>(workBuffer.data() + offset));
        }

        decryptedBatch.append(workBuffer.constData() + offset, BLOCK_SIZE);
        offset += BLOCK_SIZE;
        m_chunkIndex++;
    }

    // Save remainder for next chunk
    if (offset < workBuffer.size()) {
        m_chunkRemainder = workBuffer.mid(offset);
    }

    m_totalBytesReceived += chunk.size();

    if (!m_progressivePlaybackStarted) {
        // Accumulation phase: buffer data until we have enough to start
        m_streamBuffer.append(decryptedBatch);

        // Need at least 64KB for BASS to parse audio headers.
        if (m_streamBuffer.size() < 65536)
            return;

        // Try to create the stream. Reset read cursor so BASS reads from the start.
        // For formats with large metadata (FLAC with embedded art), BASS may need
        // more than 64KB to find the first audio frame. In that case, creation fails
        // with BASS_ERROR_FILEFORM and we keep buffering until the next chunk retries.
        m_pushInitialOffset = 0;
        BASS_FILEPROCS pushProcs = { pushStreamClose, pushStreamLength, pushStreamRead, pushStreamSeek };
        HSTREAM stream = BASS_StreamCreateFileUser(
            STREAMFILE_NOBUFFER, BASS_STREAM_DECODE,  // Let BASS auto-detect format
            &pushProcs, this);

        if (!stream) {
            int err = BASS_ErrorGetCode();
            if (err == 41 /* BASS_ERROR_FILEFORM */ && m_progressiveMode) {
                // Not enough data for format detection yet -- keep buffering
                emit debugLog(QString("[AudioEngine] Need more data for format detection (%1 bytes so far)")
                              .arg(m_streamBuffer.size()));
                return;
            }
            emit error(QString("Failed to create progressive stream: error %1").arg(err));
            m_progressiveMode.store(false);
            setState(Stopped);
            return;
        }

        m_pushStream = stream;
        m_progressivePlaybackStarted = true;

        emit debugLog(QString("[AudioEngine] Starting playback after %1 bytes buffered")
                      .arg(m_streamBuffer.size()));

        // Exclusive mode: switch WASAPI+mixer to match track sample rate
        {
            BASS_CHANNELINFO sci = {};
            if (BASS_ChannelGetInfo(m_pushStream, &sci)) {
                emit debugLog(QString("[AudioEngine] Progressive stream: %1 Hz, %2 ch, format %3")
                              .arg(sci.freq).arg(sci.chans).arg(sci.flags & BASS_SAMPLE_FLOAT ? "float" : "int"));
                emit debugLog(QString("[AudioEngine] Progressive stream sample rate: %1 Hz, current output: %2 Hz")
                              .arg(sci.freq).arg(m_outputSampleRate));
                if (!ensureOutputRate(sci.freq)) {
                    BASS_StreamFree(m_pushStream);
                    m_pushStream = 0;
                    m_progressiveMode.store(false);
                    setState(Stopped);
                    return;
                }
            }
        }

        // Temporarily disable QUEUE mode for progressive streaming.
        // In QUEUE mode, a source that runs dry (push buffer empty) gets DEQUEUED
        // permanently -- the mixer removes it and it can never resume.
        // In non-queue mode, stalled sources just produce momentary silence
        // and resume seamlessly when new data is pushed.
        BASS_ChannelFlags(m_mixerStream, 0, BASS_MIXER_QUEUE);

        // Add to mixer (plays immediately)
        BOOL ok = BASS_Mixer_StreamAddChannel(m_mixerStream, m_pushStream, 0);
        if (!ok) {
            int err = BASS_ErrorGetCode();
            emit error(QString("Failed to add stream to mixer: error %1").arg(err));
            BASS_ChannelFlags(m_mixerStream, BASS_MIXER_QUEUE, BASS_MIXER_QUEUE);
            BASS_StreamFree(m_pushStream);
            m_pushStream = 0;
            m_progressiveMode.store(false);
            setState(Stopped);
            return;
        }

        m_currentStream = m_pushStream;

        // Restore mixer queue sync
        if (!m_queueSync && m_mixerStream) {
            m_queueSync = BASS_ChannelSetSync(
                m_mixerStream, BASS_SYNC_MIXER_QUEUE, 0, queueSyncCallback, this);
        }

        // Start mixer if not already playing
        if (!isOutputActive()) {
            startMixerOutput();
        }

        emit debugLog(QString("[AudioEngine] Progressive playback started (BUFFERPUSH) after %1 bytes")
                      .arg(m_streamBuffer.size()));
        emit trackChanged(m_currentTrack);
        if (m_windowsMediaControls && m_currentTrack)
            m_windowsMediaControls->updateMetadata(
                m_currentTrack->title(), m_currentTrack->artist(),
                m_currentTrack->album(), m_currentTrack->albumArt());

        setState(Playing);
        m_positionTimer->start(100);

        // Trigger initial progressive waveform from buffered data
        {
            QByteArray wfSnapshot;
            {
                QMutexLocker locker(&m_bufferMutex);
                wfSnapshot = m_streamBuffer;
            }
            m_lastWaveformUpdateBytes = wfSnapshot.size();
            // Estimate completion ratio based on file byte rate
            double playBps = 40000;
            if (m_currentStreamFormat.contains("128")) playBps = 16000;
            else if (m_currentStreamFormat.contains("FLAC", Qt::CaseInsensitive)) playBps = 176000;
            else if (m_currentStreamFormat.contains("64")) playBps = 8000;
            int dur = (m_currentTrack && m_currentTrack->duration() > 0) ? m_currentTrack->duration() : 300;
            qint64 estTotal = static_cast<qint64>(playBps * dur);
            double ratio = (estTotal > 0) ? qMin(1.0, (double)wfSnapshot.size() / estTotal) : 1.0;
            int gen = m_waveformGeneration.load();
            auto *watcher = new QFutureWatcher<QVector<float>>(this);
            connect(watcher, &QFutureWatcher<QVector<float>>::finished, this,
                    [this, watcher, gen]() {
                if (gen == m_waveformGeneration.load()) {
                    QVector<float> peaks = watcher->result();
                    if (!peaks.isEmpty())
                        emit waveformReady(peaks);
                }
                watcher->deleteLater();
            });
            watcher->setFuture(QtConcurrent::run(computeWaveformFromBuffer, wfSnapshot, 500,
                                                &m_waveformGeneration, gen, &m_bassMutex, ratio));
        }
    } else {
        // Streaming phase: append to buffer (pushStreamRead serves it to BASS on mixer thread)
        QByteArray wfSnapshot;
        bool doWaveform = false;
        {
            QMutexLocker locker(&m_bufferMutex);
            m_streamBuffer.append(decryptedBatch);

            if (m_streamBuffer.size() - m_lastWaveformUpdateBytes >= 100000) {
                m_lastWaveformUpdateBytes = m_streamBuffer.size();
                wfSnapshot = m_streamBuffer;
                doWaveform = true;
            }
        }

        if (doWaveform) {
            // Estimate completion ratio for partial waveform
            double playBps = 40000;
            if (m_currentStreamFormat.contains("128")) playBps = 16000;
            else if (m_currentStreamFormat.contains("FLAC", Qt::CaseInsensitive)) playBps = 176000;
            else if (m_currentStreamFormat.contains("64")) playBps = 8000;
            int dur = (m_currentTrack && m_currentTrack->duration() > 0) ? m_currentTrack->duration() : 300;
            qint64 estTotal = static_cast<qint64>(playBps * dur);
            double ratio = (estTotal > 0) ? qMin(1.0, (double)wfSnapshot.size() / estTotal) : 1.0;

            int gen = m_waveformGeneration.load();
            auto *watcher = new QFutureWatcher<QVector<float>>(this);
            connect(watcher, &QFutureWatcher<QVector<float>>::finished, this,
                    [this, watcher, gen]() {
                if (gen == m_waveformGeneration.load()) {
                    QVector<float> peaks = watcher->result();
                    if (!peaks.isEmpty())
                        emit waveformReady(peaks);
                }
                watcher->deleteLater();
            });
            watcher->setFuture(QtConcurrent::run(computeWaveformFromBuffer, wfSnapshot, 500,
                                                &m_waveformGeneration, gen, &m_bassMutex, ratio));
        }
    }
}

void AudioEngine::onProgressiveDownloadFinished(const QString& errorMessage, const QString& trackId)
{
    if (!m_currentTrack || m_currentTrack->id() != trackId || !m_progressiveMode)
        return;

    if (!errorMessage.isEmpty()) {
        m_progressiveMode.store(false);
        if (errorMessage.contains("cancel", Qt::CaseInsensitive) ||
            errorMessage.contains("abort", Qt::CaseInsensitive)) {
            emit debugLog("[AudioEngine] Progressive download cancelled");
            return;
        }
        emit debugLog("[AudioEngine] Progressive download error: " + errorMessage);
        emit error(QString("Failed to load track: %1").arg(errorMessage));
        if (!m_progressivePlaybackStarted) {
            setState(Stopped);
            return;
        }
        // Download failed but playback is in progress -- treat partial data as complete:
        // append any remainder, re-enable QUEUE mode, set up syncs, update waveform.
        if (!m_chunkRemainder.isEmpty()) {
            QMutexLocker locker(&m_bufferMutex);
            m_streamBuffer.append(m_chunkRemainder);
            m_chunkRemainder.clear();
        }
        if (m_pushStream && m_mixerStream) {
            BASS_ChannelFlags(m_mixerStream, BASS_MIXER_QUEUE, BASS_MIXER_QUEUE);
            setupStreamSyncs(m_currentStream, &m_currentEndSync, &m_currentNearEndSync);
            updateStreamInfo(m_currentStream);
            startWaveformComputation();
        }
        emit debugLog(QString("[AudioEngine] Partial download: continuing playback with %1 bytes").arg(m_streamBuffer.size()));
        return;
    }

    // Append any remaining bytes (tail < 2048 is not encrypted)
    // Must append BEFORE setting m_progressiveMode to false, so pushStreamRead
    // can serve this data before it sees the EOF signal.
    if (!m_chunkRemainder.isEmpty()) {
        {
            QMutexLocker locker(&m_bufferMutex);
            m_streamBuffer.append(m_chunkRemainder);
        }
        m_chunkRemainder.clear();
    }

    // Signal EOF: pushStreamRead will return 0 once all buffered data is served
    m_progressiveMode.store(false);

    emit debugLog(QString("[AudioEngine] Progressive download complete: %1 bytes total").arg(m_streamBuffer.size()));

    if (m_progressivePlaybackStarted && m_pushStream) {
        // Re-enable QUEUE mode now that the full file is available.
        // This restores gapless transitions for preloaded next tracks.
        BASS_ChannelFlags(m_mixerStream, BASS_MIXER_QUEUE, BASS_MIXER_QUEUE);

        // Now that we have the full file, set up syncs for preloading and stream info
        setupStreamSyncs(m_currentStream, &m_currentEndSync, &m_currentNearEndSync);
        updateStreamInfo(m_currentStream);

        // Recompute waveform with ratio 1.0 (full file) to fill any gaps
        // from the estimated ratios used during progressive download
        startWaveformComputation();
    } else {
        // Small file (< 64KB): never started BUFFERPUSH playback. Use regular memory stream.
        if (m_streamBuffer.isEmpty()) {
            emit error("Failed to load track: empty response from server");
            setState(Stopped);
            return;
        }

        HSTREAM newStream = createSourceStream(m_streamBuffer);
        if (!newStream) {
            setState(Stopped);
            return;
        }

        QMutexLocker locker(&m_bassMutex);

        // Get source stream info and log it
        BASS_CHANNELINFO sci = {};
        if (BASS_ChannelGetInfo(newStream, &sci)) {
            emit debugLog(QString("[AudioEngine] Small file stream: %1 Hz, %2 ch, format %3")
                          .arg(sci.freq).arg(sci.chans).arg(sci.flags & BASS_SAMPLE_FLOAT ? "float" : "int"));

            // Exclusive mode: switch WASAPI+mixer to match track sample rate
            {
                emit debugLog(QString("[AudioEngine] Small file stream sample rate: %1 Hz, current output: %2 Hz")
                              .arg(sci.freq).arg(m_outputSampleRate));
                if (!ensureOutputRate(sci.freq)) {
                    BASS_StreamFree(newStream);
                    setState(Stopped);
                    return;
                }
            }
        }

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

        if (!m_queueSync && m_mixerStream) {
            m_queueSync = BASS_ChannelSetSync(m_mixerStream, BASS_SYNC_MIXER_QUEUE, 0, queueSyncCallback, this);
        }

        if (!isOutputActive()) {
            startMixerOutput();
        }

        locker.unlock();
        updateStreamInfo(m_currentStream);

        emit trackChanged(m_currentTrack);
        if (m_windowsMediaControls && m_currentTrack)
            m_windowsMediaControls->updateMetadata(
                m_currentTrack->title(), m_currentTrack->artist(),
                m_currentTrack->album(), m_currentTrack->albumArt());

        play();
    }

    m_listenReported = false;
    startWaveformComputation();
}

// ── Preloading ──────────────────────────────────────────────────────────

void AudioEngine::preloadNextTrack()
{
    emit debugLog("[AudioEngine] preloadNextTrack() called - START");

    int nextIndex;
    if (m_repeatMode == RepeatOne) {
        nextIndex = m_currentIndex;
        emit debugLog("[AudioEngine] Preload: repeating current track (RepeatOne mode)");
    } else {
        nextIndex = m_currentIndex + 1;
    }
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
    // (skip this check in RepeatOne mode -- we intentionally preload the same track)
    if (m_repeatMode != RepeatOne) {
        if ((m_currentTrack && m_currentTrack->id() == nextTrack->id()) ||
            (m_pendingTrack && m_pendingTrack->id() == nextTrack->id()) ||
            (m_preloadTrack && m_preloadTrack->id() == nextTrack->id()) ||
            (m_preloadStream != 0)) {
            emit debugLog(QString("[AudioEngine] Preload skipped: track '%1' matches current, pending, or already preloaded").arg(nextTrack->title()));
            return;
        }
    } else if (m_preloadStream != 0) {
        emit debugLog("[AudioEngine] RepeatOne: already preloading, skipping");
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
        QString streamId = nextTrack->isUserUploaded() ? nextTrack->trackToken() : nextTrack->id();
        QString streamFormat = nextTrack->isUserUploaded() ? QStringLiteral("MP3_MISC") : QString();
        m_deezerAPI->getStreamUrl(streamId, nextTrack->trackToken(), streamFormat);
        emit debugLog("[AudioEngine] getStreamUrl call completed");
    } else {
        emit debugLog("[AudioEngine] ERROR: DeezerAPI is null!");
    }

    emit debugLog("[AudioEngine] preloadNextTrack() - END");
}

// ── Preload progressive download handlers ───────────────────────────────
// Preload downloads accumulate raw (encrypted) chunks, then decrypt the
// full buffer on completion -- same end result as the old startDownload
// path but using the progressive download mechanism.

void AudioEngine::onPreloadChunkReady(const QByteArray& chunk, const QString& trackId)
{
    auto matchPreloadId = [&]() {
        if (!m_preloadTrack) return false;
        return m_preloadTrack->isUserUploaded() ? (m_preloadTrack->trackToken() == trackId) : (m_preloadTrack->id() == trackId);
    };
    if (!matchPreloadId()) return;

    m_preloadBuffer.append(chunk);
}

void AudioEngine::onPreloadDownloadFinished(const QString& errorMessage, const QString& trackId)
{
    auto matchPreloadId = [&]() {
        if (!m_preloadTrack) return false;
        return m_preloadTrack->isUserUploaded() ? (m_preloadTrack->trackToken() == trackId) : (m_preloadTrack->id() == trackId);
    };
    if (!matchPreloadId()) return;

    if (!errorMessage.isEmpty()) {
        if (errorMessage.contains("cancel", Qt::CaseInsensitive) ||
            errorMessage.contains("abort", Qt::CaseInsensitive)) {
            emit debugLog("[AudioEngine] Preload download cancelled");
            m_preloadTrack.reset();
        } else {
            emit debugLog("[AudioEngine] Preload download failed: " + errorMessage);
            m_preloadTrack.reset();
        }
        return;
    }

    if (m_preloadBuffer.isEmpty()) {
        emit debugLog("[AudioEngine] Preload download empty");
        m_preloadTrack.reset();
        return;
    }

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

        if (!m_mixerStream) {
            emit debugLog("[AudioEngine] ERROR: Cannot queue - mixer stream is null");
            BASS_StreamFree(nextStream);
            return;
        }

        BOOL ok = BASS_Mixer_StreamAddChannel(
            m_mixerStream,
            nextStream,
            BASS_MIXER_CHAN_NORAMPIN | BASS_STREAM_AUTOFREE
        );

        if (!ok) {
            int err = BASS_ErrorGetCode();
            emit debugLog(QString("[AudioEngine] Failed to add next stream to mixer: %1").arg(err));
            BASS_StreamFree(nextStream);
            return;
        }

        m_preloadStream = nextStream;
        QString trackTitle = m_preloadTrack ? m_preloadTrack->title() : "Unknown";
        locker.unlock();

        emit debugLog(QString("[AudioEngine] Next track ready for gapless playback: %1").arg(trackTitle));
    } else {
        emit debugLog("[AudioEngine] ERROR: createSourceStream returned null for preload");
    }
}
