#include "playlist.h"

Playlist::Playlist(QObject *parent)
    : QObject(parent)
{
}

Playlist::Playlist(const QString& id, const QString& title, QObject *parent)
    : QObject(parent)
    , m_id(id)
    , m_title(title)
{
}

void Playlist::addTrack(std::shared_ptr<Track> track)
{
    m_tracks.append(track);
    emit trackAdded(track);
}

void Playlist::removeTrack(int index)
{
    if (index >= 0 && index < m_tracks.size()) {
        m_tracks.removeAt(index);
        emit trackRemoved(index);
    }
}

void Playlist::clearTracks()
{
    m_tracks.clear();
    emit playlistCleared();
}

std::shared_ptr<Track> Playlist::getTrack(int index) const
{
    if (index >= 0 && index < m_tracks.size()) {
        return m_tracks[index];
    }
    return nullptr;
}
