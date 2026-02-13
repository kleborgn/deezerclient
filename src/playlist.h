#ifndef PLAYLIST_H
#define PLAYLIST_H

#include <QObject>
#include <QList>
#include <QDateTime>
#include <memory>
#include "track.h"

class Playlist : public QObject
{
    Q_OBJECT

public:
    explicit Playlist(QObject *parent = nullptr);
    Playlist(const QString& id, const QString& title, QObject *parent = nullptr);

    // Getters
    QString id() const { return m_id; }
    QString title() const { return m_title; }
    QString description() const { return m_description; }
    int trackCount() const { return m_tracks.isEmpty() ? m_trackCount : m_tracks.size(); }
    void setTrackCount(int count) { m_trackCount = count; }
    
    // Track management
    void addTrack(std::shared_ptr<Track> track);
    void removeTrack(int index);
    void clearTracks();
    std::shared_ptr<Track> getTrack(int index) const;
    const QList<std::shared_ptr<Track>>& tracks() const { return m_tracks; }

    // Setters
    void setId(const QString& id) { m_id = id; }
    void setTitle(const QString& title) { m_title = title; }
    void setDescription(const QString& desc) { m_description = desc; }
    void setCoverUrl(const QString& url) { m_coverUrl = url; }
    QDateTime lastModified() const { return m_lastModified; }
    void setLastModified(const QDateTime& dt) { m_lastModified = dt; }

    QString coverUrl() const { return m_coverUrl; }
    int totalDuration() const { return m_totalDuration; }
    void setTotalDuration(int duration) { m_totalDuration = duration; }

signals:
    void trackAdded(std::shared_ptr<Track> track);
    void trackRemoved(int index);
    void playlistCleared();

private:
    QString m_id;
    QString m_title;
    QString m_description;
    QString m_coverUrl;
    int m_totalDuration = 0;
    int m_trackCount = 0;
    QDateTime m_lastModified;
    QList<std::shared_ptr<Track>> m_tracks;
};

#endif // PLAYLIST_H
