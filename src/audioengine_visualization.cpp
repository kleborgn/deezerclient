#include "audioengine.h"
#include "deezerapi.h"
#include <QTimer>
#include <QtConcurrent>
#include <QFutureWatcher>

extern "C" {
#include "bassmix.h"
#include "basswasapi.h"
}

// ── Waveform computation (runs on thread-pool, never blocks the UI) ─────────

// Thread-safe free function: takes a copy of the audio buffer (QByteArray COW)
// and returns normalised peak amplitudes. Only uses its own BASS decode handle.
// Aborts if currentGeneration != *generationPtr
QVector<float> computeWaveformFromBuffer(const QByteArray& data, int numPeaks,
                                               std::atomic<int>* generationPtr, int currentGeneration,
                                               QRecursiveMutex* bassMutex,
                                               double completionRatio = 1.0)
{
    if (data.isEmpty() || numPeaks <= 0)
        return {};

    // The decode stream is private to this function, so mutex is only needed for
    // creation and destruction (BASS global state), not for reads.
    // Note: avoid BASS_SAMPLE_MONO — it causes BASS_ChannelGetLength to report
    // incorrect byte counts for some formats (FLAC), producing half-length waveforms.
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
        return {};

    QWORD totalBytes = BASS_ChannelGetLength(decode, BASS_POS_BYTE);
    if (totalBytes == 0 || totalBytes == static_cast<QWORD>(-1)) {
        QMutexLocker locker(bassMutex);
        BASS_StreamFree(decode);
        return {};
    }

    // Estimate the full decoded length so each peak always maps to the same
    // absolute song position.  During progressive download totalBytes only
    // covers the data received so far; dividing by completionRatio gives the
    // expected full length.  This keeps existing peaks stable as new data
    // arrives — new peaks simply fill in from the right.
    QWORD estimatedFullBytes = (completionRatio > 0.01)
        ? static_cast<QWORD>(totalBytes / completionRatio)
        : totalBytes;

    QVector<float> peaks(numPeaks, 0.0f);
    QVector<int>   counts(numPeaks, 0);
    const double bytesPerPeak = static_cast<double>(estimatedFullBytes) / numPeaks;

    // Single sequential decode pass — no seeking.  BASS_ChannelSetPosition on
    // in-memory decode streams re-decodes from the start to each target, so
    // 500 seeks ≈ 250 full decodes.  A single forward pass is much faster.
    // We read large chunks and accumulate the average absolute value per bucket.
    static constexpr int BUF_SAMPLES = 16384;
    float buffer[BUF_SAMPLES];
    QWORD decodedBytes = 0;
    int readCount = 0;

    while (true) {
        // Abort check every ~50 reads (~3.2 MB of mono float)
        if ((++readCount % 50 == 0) && generationPtr && generationPtr->load() != currentGeneration) {
            QMutexLocker locker(bassMutex);
            BASS_StreamFree(decode);
            return {};
        }

        DWORD bytesRead = BASS_ChannelGetData(decode, buffer,
                                               BUF_SAMPLES * sizeof(float));
        if (bytesRead == static_cast<DWORD>(-1) || bytesRead == 0)
            break;

        int samples = static_cast<int>(bytesRead / sizeof(float));
        for (int s = 0; s < samples; ++s) {
            int peakIdx = static_cast<int>((decodedBytes + s * sizeof(float)) / bytesPerPeak);
            if (peakIdx >= numPeaks) goto done;
            peaks[peakIdx] += qAbs(buffer[s]);
            counts[peakIdx]++;
        }
        decodedBytes += bytesRead;
    }
    done:

    // Convert sums to averages
    for (int i = 0; i < numPeaks; ++i) {
        if (counts[i] > 0)
            peaks[i] /= counts[i];
    }

    {
        QMutexLocker locker(bassMutex);
        BASS_StreamFree(decode);
    }

    // Normalise peaks to 0.0 - 1.0 and apply power curve for visual dynamic range
    float maxPeak = *std::max_element(peaks.begin(), peaks.end());
    if (maxPeak > 0.0f) {
        for (float& p : peaks) {
            p /= maxPeak;
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
    // the COW copy detaches safely - no data race.
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

    watcher->setFuture(QtConcurrent::run(computeWaveformFromBuffer, bufferSnapshot, 500, &m_waveformGeneration, generation, &m_bassMutex, 1.0));
}

// ── Position & Duration Tracking ────────────────────────────────────────

double AudioEngine::position() const
{
    QMutexLocker locker(&m_bassMutex);
    if (!m_initialized || !m_mixerStream || !m_currentStream) {
        return 0.0;
    }

    QWORD pos = BASS_Mixer_ChannelGetPosition(m_currentStream, BASS_POS_BYTE);
    if (pos == (QWORD)-1) return 0.0;

    QWORD length = BASS_ChannelGetLength(m_currentStream, BASS_POS_BYTE);

    if (length == 0 || length == (QWORD)-1 || m_pushStream != 0) {
        // Length unknown or unreliable (progressive push stream has fake length) -- use track metadata
        if (m_currentTrack && m_currentTrack->duration() > 0) {
            double seconds = BASS_ChannelBytes2Seconds(m_currentStream, pos);
            return qBound(0.0, seconds / m_currentTrack->duration(), 1.0);
        }
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
        if (active != BASS_ACTIVE_PLAYING && active != BASS_ACTIVE_PAUSED
            && active != BASS_ACTIVE_STALLED) {
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
        if (m_currentTrack)
            return m_currentTrack->duration();
        return 0;
    }

    QWORD length = BASS_ChannelGetLength(m_currentStream, BASS_POS_BYTE);
    if (length == (QWORD)-1 || m_pushStream != 0) {
        // Stream length unknown or unreliable (progressive push stream has fake length) -- use track metadata
        if (m_currentTrack)
            return m_currentTrack->duration();
        return 0;
    }
    double seconds = BASS_ChannelBytes2Seconds(m_currentStream, length);

    return static_cast<int>(seconds);
}

void AudioEngine::updatePosition()
{
    int currentSeconds = positionSeconds();

    if (currentSeconds != m_lastPositionSeconds) {
        m_lastPositionSeconds = currentSeconds;
        emit positionChanged(currentSeconds);

        // Report play to Deezer after 30 seconds (skip user-uploaded tracks)
        if (currentSeconds >= 30 && !m_listenReported && m_currentTrack && !m_currentTrack->isUserUploaded()) {
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

// ── Spectrum Analysis ───────────────────────────────────────────────────

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

    // CRITICAL: In WASAPI mode the mixer is a decode channel. BASS_ChannelGetData
    // on a decode channel CONSUMES data from the decode pipeline, stealing it from
    // WASAPI output and causing broken/choppy audio. Use BASS_WASAPI_GetData instead
    // which reads from the WASAPI output buffer without consuming decode data.
    bool useWasapi = (m_outputMode != OutputDirectSound);

    DWORD result = useWasapi
        ? BASS_WASAPI_GetData(pcmInterleaved, BASS_DATA_FLOAT | PCM_SAMPLES * 2 * sizeof(float))
        : BASS_ChannelGetData(m_mixerStream, pcmInterleaved, BASS_DATA_FLOAT | PCM_SAMPLES * 2 * sizeof(float));

    if (result == (DWORD)-1) {
        if (callCount <= 3) {
            emit debugLog(QString("[AudioEngine] %1 failed, error code: %2")
                         .arg(useWasapi ? "BASS_WASAPI_GetData" : "BASS_ChannelGetData")
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
    result = useWasapi
        ? BASS_WASAPI_GetData(fft, BASS_DATA_FFT8192)
        : BASS_ChannelGetData(m_mixerStream, fft, BASS_DATA_FFT8192);

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
