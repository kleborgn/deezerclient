#include "album.h"

Album::Album(QObject *parent)
    : QObject(parent)
{
}

Album::Album(const QString& id, const QString& title, const QString& artist, QObject *parent)
    : QObject(parent)
    , m_id(id)
    , m_title(title)
    , m_artist(artist)
{
}
