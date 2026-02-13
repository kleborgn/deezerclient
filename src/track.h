#ifndef TRACK_H
#define TRACK_H

#include <QString>
#include <QObject>
#include <QJsonArray>

class Track : public QObject
{
    Q_OBJECT

public:
    explicit Track(QObject *parent = nullptr);
    Track(const QString& id, const QString& title, const QString& artist,
          const QString& album, int duration, const QString& previewUrl,
          const QString& albumArt, QObject *parent = nullptr);

    // Getters
    QString id() const { return m_id; }
    QString title() const { return m_title; }
    QString artist() const { return m_artist; }
    QString album() const { return m_album; }
    int duration() const { return m_duration; }
    QString previewUrl() const { return m_previewUrl; }
    QString albumArt() const { return m_albumArt; }
    QString streamUrl() const { return m_streamUrl; }
    QString trackToken() const { return m_trackToken; }
    QString lyrics() const { return m_lyrics; }
    QJsonArray syncedLyrics() const { return m_syncedLyrics; }
    int scrobbleCount() const { return m_scrobbleCount; }
    int userScrobbleCount() const { return m_userScrobbleCount; }
    bool hasScrobbleData() const { return m_scrobbleCount >= 0; }
    bool isFavorite() const { return m_isFavorite; }

    // Setters
    void setId(const QString& id) { m_id = id; }
    void setTitle(const QString& title) { m_title = title; }
    void setArtist(const QString& artist) { m_artist = artist; }
    void setAlbum(const QString& album) { m_album = album; }
    void setDuration(int duration) { m_duration = duration; }
    void setPreviewUrl(const QString& url) { m_previewUrl = url; }
    void setAlbumArt(const QString& url) { m_albumArt = url; }
    void setStreamUrl(const QString& url) { m_streamUrl = url; }
    void setTrackToken(const QString& token) { m_trackToken = token; }
    void setLyrics(const QString& lyrics) { m_lyrics = lyrics; }
    void setSyncedLyrics(const QJsonArray& syncedLyrics) { m_syncedLyrics = syncedLyrics; }
    void setScrobbleCount(int count) { m_scrobbleCount = count; }
    void setUserScrobbleCount(int count) { m_userScrobbleCount = count; }
    void setFavorite(bool fav) { m_isFavorite = fav; }

    // Helper methods
    QString durationString() const;

private:
    QString m_id;
    QString m_title;
    QString m_artist;
    QString m_album;
    int m_duration; // in seconds
    QString m_previewUrl;
    QString m_albumArt;
    QString m_streamUrl; // Full quality stream URL (requires authentication)
    QString m_trackToken; // TRACK_TOKEN for media API (get_url)
    QString m_lyrics; // Plain text lyrics
    QJsonArray m_syncedLyrics; // Synchronized lyrics with timestamps
    int m_scrobbleCount = -1; // Last.fm scrobble count (-1 = not fetched, 0+ = actual count)
    int m_userScrobbleCount = -1; // User's personal scrobble count
    bool m_isFavorite = false;
};

#endif // TRACK_H
