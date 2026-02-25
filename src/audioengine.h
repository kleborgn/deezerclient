#ifndef AUDIOENGINE_H
#define AUDIOENGINE_H

#include <QObject>
#include <QQueue>
#include <QByteArray>
#include <QVector>
#include <QMutex>
#include <QRecursiveMutex>
#include <atomic>
#include <memory>
#include <QElapsedTimer>
#include "track.h"

class QTimer;
class QThread;
class DeezerAPI;
class StreamDownloader;

// BASS types for callback declarations (bass.h uses extern "C" when included from C++)
#include "bass.h"

class AudioEngine : public QObject
{
    Q_OBJECT

public:
    enum PlaybackState {
        Stopped,
        Playing,
        Paused,
        Loading
    };

    enum RepeatMode {
        RepeatOff,
        RepeatOne,
        RepeatAll
    };

    enum OutputMode {
        OutputDirectSound,
        OutputWasapiShared,
        OutputWasapiExclusive
    };

    struct AudioDevice {
        int index;
        QString name;
        DWORD mixfreq;
        DWORD mixchans;
        DWORD type;
        bool isDefault;
    };

    explicit AudioEngine(QObject *parent = nullptr);
    ~AudioEngine();

    // Initialization
    bool initialize();
    void shutdown();

    // Playback control
    void play();
    void pause();
    void stop();
    void seek(double position); // 0.0 to 1.0
    
    // Track management
    void setDeezerAPI(DeezerAPI* api);
    void loadTrack(std::shared_ptr<Track> track);
    void setQueue(const QList<std::shared_ptr<Track> >& tracks);
    void setQueue(const QList<std::shared_ptr<Track> >& tracks, const QString& contextType, const QString& contextId);
    void playAtIndex(int index);
    void next();
    void previous();

    // Queue management
    void removeFromQueue(int index);
    void removeFromQueue(const QList<int>& indices);
    void moveInQueue(int fromIndex, int toIndex);
    void addToQueue(std::shared_ptr<Track> track, int position = -1);  // -1 = append
    void addToQueue(const QList<std::shared_ptr<Track>>& tracks, int position = -1);
    void clearQueue();

    // Repeat mode
    void setRepeatMode(RepeatMode mode);
    RepeatMode repeatMode() const { return m_repeatMode; }
    
    // Volume control (0.0 to 1.0)
    void setVolume(float volume);
    float volume() const { return m_volume; }
    
    // Getters
    PlaybackState state() const { return m_state; }
    std::shared_ptr<Track> currentTrack() const { 
        QMutexLocker locker(&m_bassMutex);
        return m_currentTrack; 
    }
    int currentIndex() const { return m_currentIndex; }
    QString contextType() const { return m_contextType; }
    QString contextId() const { return m_contextId; }
    QList<std::shared_ptr<Track>> queue() const;
    int positionSeconds() const;
    int durationSeconds() const;
    
    // Gapless playback settings
    void setGaplessEnabled(bool enabled) { m_gaplessEnabled = enabled; }

    // Manual preloading (e.g., triggered by UI hover)
    void preloadNextTrack();
    bool isNextPreloaded() const { return m_preloadReady; }

    // Output mode (DirectSound / WASAPI Shared / WASAPI Exclusive)
    void setOutputMode(OutputMode mode, int wasapiDevice = -1);
    bool reinitialize(OutputMode mode, int wasapiDevice = -1);
    OutputMode outputMode() const { return m_outputMode; }
    int wasapiDeviceIndex() const { return m_wasapiDevice; }
    DWORD outputSampleRate() const { return m_outputSampleRate; }
    static QList<AudioDevice> enumerateWasapiDevices();

signals:
    void stateChanged(PlaybackState state);
    void trackChanged(std::shared_ptr<Track> track);
    void queueChanged();
    void positionChanged(int seconds);
    void streamInfoChanged(const QString& info); // e.g. "FLAC | 1411 kbps | 44100 Hz | stereo"
    void waveformReady(const QVector<float>& peaks);
    void positionTick(double position); // 0.0-1.0, emitted every ~100ms for smooth waveform playhead
    void repeatModeChanged(AudioEngine::RepeatMode mode);
    void spectrumDataReady(const QVector<float>& magnitudes); // Spectrum analyzer data
    void pcmDataReady(const std::vector<float>& leftChannel, const std::vector<float>& rightChannel); // Raw PCM samples for visualizer
    void error(const QString& message);
    void debugLog(const QString& message);

public slots:
    void onStreamUrlReceived(const QString& trackId, const QString& url, const QString& format);
    void setSpectrumEnabled(bool enabled);

private slots:
    void updatePosition();
    void updateSpectrum();
    void onStreamChunkReady(const QByteArray& chunk, const QString& trackId);
    void onProgressiveDownloadFinished(const QString& errorMessage, const QString& trackId);
    void onPreloadChunkReady(const QByteArray& chunk, const QString& trackId);
    void onPreloadDownloadFinished(const QString& errorMessage, const QString& trackId);
    void handleStreamEnd(DWORD streamHandle);
    void handleNearEnd();
    void handleStreamDequeued(DWORD streamHandle, int generation);

private:
    // BASS callbacks
    static void CALLBACK syncEndCallback(HSYNC handle, DWORD channel, DWORD data, void* user);
    static void CALLBACK syncNearEndCallback(HSYNC handle, DWORD channel, DWORD data, void* user);
    static void CALLBACK queueSyncCallback(HSYNC handle, DWORD channel, DWORD data, void* user);

    // BASS FILEPROCS for STREAMFILE_BUFFERPUSH (progressive streaming)
    static void CALLBACK pushStreamClose(void* user);
    static QWORD CALLBACK pushStreamLength(void* user);
    static DWORD CALLBACK pushStreamRead(void* buffer, DWORD length, void* user);
    static BOOL CALLBACK pushStreamSeek(QWORD offset, void* user);

    // Internal methods
    double position() const; // 0.0 to 1.0 (used internally by updatePosition, reinitialize)
    void setState(PlaybackState state);
    void startLoadingUrl(const QString& url);
    bool createStream(const QString& url);
    HSTREAM createSourceStream(const QByteArray& data);
    void addStreamToMixer(const QByteArray& data);
    void updateStreamInfo(HSTREAM stream);
    void setupStreamSyncs(HSTREAM stream, HSYNC* endSyncPtr, HSYNC* nearEndSyncPtr);
    void destroyStream();
    void startWaveformComputation();

    // Output mode helpers (abstract DirectSound vs WASAPI)
    bool startMixerOutput();   // BASS_ChannelPlay or BASS_WASAPI_Start
    bool isOutputActive();     // BASS_ChannelIsActive or BASS_WASAPI_IsStarted
    void stopMixerOutput();    // BASS_ChannelStop or BASS_WASAPI_Stop
    bool ensureOutputRate(DWORD sourceFreq);  // Exclusive mode: switch WASAPI+mixer to match track rate
    
    HSTREAM m_mixerStream;        // Persistent mixer output with BASS_MIXER_QUEUE
    HSTREAM m_currentStream;      // Currently playing source (for position tracking)
    HSYNC m_queueSync;           // BASS_SYNC_MIXER_QUEUE sync on mixer
    int m_dequeueGeneration = 0; // Incremented in destroyStream to discard stale callbacks
    bool m_currentStreamAdded;    // Flag to track if current stream was newly added (not preloaded)
    HSYNC m_currentEndSync;      // END sync on current stream (for near-end preloading)
    HSYNC m_currentNearEndSync;  // NEAR_END sync on current stream (for preloading)
    
    DeezerAPI* m_deezerAPI;
    std::shared_ptr<Track> m_currentTrack;
    std::shared_ptr<Track> m_pendingTrack;
    QList<std::shared_ptr<Track> > m_queue;
    int m_currentIndex;
    RepeatMode m_repeatMode;

    // Context tracking for log.listen (album/playlist source)
    QString m_contextType;  // e.g., "album_page" or "profile_playlists"
    QString m_contextId;    // Album or playlist ID
    
    PlaybackState m_state;
    float m_volume;
    bool m_gaplessEnabled;
    bool m_initialized;
    
    QTimer* m_positionTimer;
    QTimer* m_spectrumTimer;
    int m_lastPositionSeconds;
    bool m_spectrumEnabled;

    // HTTPS download in worker thread so main thread never blocks (DNS/SSL)
    QThread* m_downloadThread;
    StreamDownloader* m_streamDownloader;
    StreamDownloader* m_preloadDownloader;
    QByteArray m_streamBuffer;
    QString m_currentStreamFormat;  // Format from streamUrlReceived (e.g. MP3_320), for debug file name

    // Preloading: download the next track before the current one ends
    std::shared_ptr<Track> m_preloadTrack;
    QByteArray m_preloadBuffer;
    QString m_preloadFormat;
    bool m_preloadReady = false;
    HSTREAM m_preloadStream;  // Track the preloaded stream handle for gapless playback
    bool m_listenReported = false;

    // Progressive streaming state
    HSTREAM m_pushStream = 0;
    QByteArray m_trackKey;
    QByteArray m_chunkRemainder;
    int m_chunkIndex = 0;
    int m_pushInitialOffset = 0;  // Read cursor into m_streamBuffer for STREAMFILE_BUFFER callback
    std::atomic<bool> m_progressiveMode{false};
    bool m_progressivePlaybackStarted = false;
    qint64 m_totalBytesReceived = 0;
    QElapsedTimer m_downloadTimer;  // For bandwidth estimation
    QMutex m_bufferMutex;  // Protects m_streamBuffer between main thread and BASS mixing thread
    qint64 m_lastWaveformUpdateBytes = 0;  // Track when to trigger next progressive waveform update

    // Output mode
    OutputMode m_outputMode;
    int m_wasapiDevice;
    DWORD m_outputSampleRate;

    // Waveform computation (runs on thread pool, generation counter prevents stale results)
    std::atomic<int> m_waveformGeneration{0};
    

    // Recursive mutex for thread-safe access to BASS from multiple threads (e.g. waveform worker)
    // Allows nested locking in setupStreamSyncs() and other methods
    mutable QRecursiveMutex m_bassMutex;

    class WindowsMediaControls* m_windowsMediaControls;
};

#endif // AUDIOENGINE_H
