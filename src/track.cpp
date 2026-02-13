#include "track.h"

Track::Track(QObject *parent)
    : QObject(parent)
    , m_duration(0)
{
}

Track::Track(const QString& id, const QString& title, const QString& artist,
             const QString& album, int duration, const QString& previewUrl,
             const QString& albumArt, QObject *parent)
    : QObject(parent)
    , m_id(id)
    , m_title(title)
    , m_artist(artist)
    , m_album(album)
    , m_duration(duration)
    , m_previewUrl(previewUrl)
    , m_albumArt(albumArt)
{
}

QString Track::durationString() const
{
    int minutes = m_duration / 60;
    int seconds = m_duration % 60;
    return QString("%1:%2").arg(minutes).arg(seconds, 2, 10, QChar('0'));
}
