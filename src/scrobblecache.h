#ifndef SCROBBLECACHE_H
#define SCROBBLECACHE_H

#include <QObject>
#include <QMap>
#include <QString>
#include <QDateTime>

/**
 * In-memory cache for Last.fm scrobble counts.
 * Caches track and album playcount data with a 24-hour TTL to avoid excessive API calls.
 */
struct ScrobbleData {
    int playcount = 0;
    int userPlaycount = 0;
    QDateTime timestamp;
    bool isValid = false;
};

class ScrobbleCache : public QObject
{
    Q_OBJECT

public:
    explicit ScrobbleCache(QObject *parent = nullptr);

    // Track cache
    void setTrackPlaycount(const QString& artist, const QString& track, int playcount, int userPlaycount);
    ScrobbleData getTrackPlaycount(const QString& artist, const QString& track) const;
    bool hasTrackData(const QString& artist, const QString& track) const;

    // Album cache
    void setAlbumPlaycount(const QString& artist, const QString& album, int playcount, int userPlaycount);
    ScrobbleData getAlbumPlaycount(const QString& artist, const QString& album) const;
    bool hasAlbumData(const QString& artist, const QString& album) const;

    // Cache management
    void clear();
    void clearExpired();

private:
    QString makeTrackKey(const QString& artist, const QString& track) const;
    QString makeAlbumKey(const QString& artist, const QString& album) const;
    bool isCacheExpired(const QDateTime& timestamp) const;

    QMap<QString, ScrobbleData> m_trackCache;
    QMap<QString, ScrobbleData> m_albumCache;

    static const int CACHE_EXPIRY_HOURS = 24;
};

#endif // SCROBBLECACHE_H
