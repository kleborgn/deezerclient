#include "audioengine.h"
#include "streamdownloader.h"
#include <QMetaObject>

// ── Queue Management Methods ────────────────────────────────────────────

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

    if (m_repeatMode == RepeatOne) {
        // Replay the current track
        loadTrack(m_queue[m_currentIndex]);
        emit queueChanged();
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

void AudioEngine::removeFromQueue(int index)
{
    if (index < 0 || index >= m_queue.size())
        return;

    // Case 1: Removing track before current -> decrement m_currentIndex
    if (index < m_currentIndex) {
        m_queue.removeAt(index);
        m_currentIndex--;
        emit queueChanged();
        return;
    }

    // Case 2: Removing current track -> stop and restart playback
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
        QMetaObject::invokeMethod(m_preloadDownloader, "startProgressiveDownload",
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
                    QMetaObject::invokeMethod(m_preloadDownloader, "startProgressiveDownload",
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
