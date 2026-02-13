#ifndef ALBUM_H
#define ALBUM_H

#include <QString>
#include <QObject>
#include <memory>
#include <QList>
#include "track.h"

class Album : public QObject
{
    Q_OBJECT

public:
    explicit Album(QObject *parent = nullptr);
    Album(const QString& id, const QString& title, const QString& artist, QObject *parent = nullptr);

    // Getters
    QString id() const { return m_id; }
    QString title() const { return m_title; }
    QString artist() const { return m_artist; }
    QString coverUrl() const { return m_coverUrl; }
    QString releaseDate() const { return m_releaseDate; }
    int totalDuration() const { return m_totalDuration; }
    int trackCount() const { return m_trackCount; }
    const QList<std::shared_ptr<Track>>& tracks() const { return m_tracks; }

    // Setters
    void setId(const QString& id) { m_id = id; }
    void setTitle(const QString& title) { m_title = title; }
    void setArtist(const QString& artist) { m_artist = artist; }
    void setCoverUrl(const QString& url) { m_coverUrl = url; }
    void setReleaseDate(const QString& date) { m_releaseDate = date; }
    void setTotalDuration(int duration) { m_totalDuration = duration; }
    void setTrackCount(int count) { m_trackCount = count; }
    void setTracks(const QList<std::shared_ptr<Track>>& tracks) { m_tracks = tracks; }

private:
    QString m_id;
    QString m_title;
    QString m_artist;
    QString m_coverUrl;
    QString m_releaseDate;
    int m_totalDuration = 0;
    int m_trackCount = 0;
    QList<std::shared_ptr<Track>> m_tracks;
};

#endif // ALBUM_H
