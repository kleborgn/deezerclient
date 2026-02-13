#include "scrobblecache.h"
#include <QDebug>

ScrobbleCache::ScrobbleCache(QObject *parent)
    : QObject(parent)
{
}

void ScrobbleCache::setTrackPlaycount(const QString& artist, const QString& track, int playcount, int userPlaycount)
{
    QString key = makeTrackKey(artist, track);

    ScrobbleData data;
    data.playcount = playcount;
    data.userPlaycount = userPlaycount;
    data.timestamp = QDateTime::currentDateTime();
    data.isValid = true;

    m_trackCache[key] = data;
}

ScrobbleData ScrobbleCache::getTrackPlaycount(const QString& artist, const QString& track) const
{
    QString key = makeTrackKey(artist, track);

    if (m_trackCache.contains(key)) {
        const ScrobbleData& data = m_trackCache[key];
        if (!isCacheExpired(data.timestamp)) {
            return data;
        }
    }

    ScrobbleData invalid;
    invalid.isValid = false;
    return invalid;
}

bool ScrobbleCache::hasTrackData(const QString& artist, const QString& track) const
{
    QString key = makeTrackKey(artist, track);

    if (m_trackCache.contains(key)) {
        const ScrobbleData& data = m_trackCache[key];
        return !isCacheExpired(data.timestamp);
    }

    return false;
}

void ScrobbleCache::setAlbumPlaycount(const QString& artist, const QString& album, int playcount, int userPlaycount)
{
    QString key = makeAlbumKey(artist, album);

    ScrobbleData data;
    data.playcount = playcount;
    data.userPlaycount = userPlaycount;
    data.timestamp = QDateTime::currentDateTime();
    data.isValid = true;

    m_albumCache[key] = data;
}

ScrobbleData ScrobbleCache::getAlbumPlaycount(const QString& artist, const QString& album) const
{
    QString key = makeAlbumKey(artist, album);

    if (m_albumCache.contains(key)) {
        const ScrobbleData& data = m_albumCache[key];
        if (!isCacheExpired(data.timestamp)) {
            return data;
        }
    }

    ScrobbleData invalid;
    invalid.isValid = false;
    return invalid;
}

bool ScrobbleCache::hasAlbumData(const QString& artist, const QString& album) const
{
    QString key = makeAlbumKey(artist, album);

    if (m_albumCache.contains(key)) {
        const ScrobbleData& data = m_albumCache[key];
        return !isCacheExpired(data.timestamp);
    }

    return false;
}

void ScrobbleCache::clear()
{
    m_trackCache.clear();
    m_albumCache.clear();
    qDebug() << "[ScrobbleCache] Cache cleared";
}

void ScrobbleCache::clearExpired()
{
    // Remove expired track entries
    QMutableMapIterator<QString, ScrobbleData> trackIt(m_trackCache);
    int removedTracks = 0;
    while (trackIt.hasNext()) {
        trackIt.next();
        if (isCacheExpired(trackIt.value().timestamp)) {
            trackIt.remove();
            removedTracks++;
        }
    }

    // Remove expired album entries
    QMutableMapIterator<QString, ScrobbleData> albumIt(m_albumCache);
    int removedAlbums = 0;
    while (albumIt.hasNext()) {
        albumIt.next();
        if (isCacheExpired(albumIt.value().timestamp)) {
            albumIt.remove();
            removedAlbums++;
        }
    }

    if (removedTracks > 0 || removedAlbums > 0) {
        qDebug() << "[ScrobbleCache] Cleared" << removedTracks << "expired tracks and" << removedAlbums << "expired albums";
    }
}

QString ScrobbleCache::makeTrackKey(const QString& artist, const QString& track) const
{
    return (artist.trimmed() + "|" + track.trimmed()).toLower();
}

QString ScrobbleCache::makeAlbumKey(const QString& artist, const QString& album) const
{
    return (artist.trimmed() + "|" + album.trimmed()).toLower();
}

bool ScrobbleCache::isCacheExpired(const QDateTime& timestamp) const
{
    QDateTime now = QDateTime::currentDateTime();
    qint64 hoursDiff = timestamp.secsTo(now) / 3600;
    return hoursDiff >= CACHE_EXPIRY_HOURS;
}
