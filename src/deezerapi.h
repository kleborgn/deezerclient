#ifndef DEEZERAPI_H
#define DEEZERAPI_H

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrlQuery>
#include <QCryptographicHash>
#include <QSet>
#include <memory>
#include "track.h"
#include "playlist.h"
#include "album.h"
#include "deezerauth.h"

/**
 * Deezer API client aligned with diezel (https://github.com/svbnet/diezel).
 * Uses the mobile Gateway API: https://api.deezer.com/1.0/gateway.php
 * Query: api_key, output=3, input=3 (POST), method, sid (when authenticated).
 * POST body = params object only (no JSON-RPC wrapper).
 */
class DeezerAPI : public QObject
{
    Q_OBJECT

public:
    explicit DeezerAPI(QObject *parent = nullptr);
    ~DeezerAPI();

    // Authentication (diezel MobileClient style)
    void signInWithEmail(const QString& email, const QString& password);
    void signInWithArl(const QString& arl);       // Uses mobile_userAutoLog (need user id from web first)
    void logout();
    bool isAuthenticated() const;
    QString sid() const;
    QString arl() const;
    QString checkForm() const;

    // Keys (diezel: MOBILE_API_KEY, TRACK_XOR_KEY)
    static void setApiKey(const QString& apiKey);       // MOBILE_API_KEY
    static void setMobileGwKey(const QString& key);   // MOBILE_GW_KEY (for initializeKeys / email login) - forwards to DeezerAuth
    static void setTrackXorKey(const QByteArray& key);  // TRACK_XOR_KEY (16 bytes) for stream decryption (BF_CBC_STRIPE)
    static void setTrackXorKey(const QString& key);     // 16-char raw or 32-char hex
    static void setLicenseToken(const QString& token); // Optional override after login
    static QString apiKey() { return s_mobileApiKey; }
    // Decrypt BF_CBC_STRIPE stream in-place. Returns true if decryption was applied (key set), false otherwise.
    bool decryptStreamBuffer(QByteArray& data, const QString& trackId) const;

    // Get auth instance for advanced usage
    DeezerAuth* auth() const { return m_auth; }

    // Search (diezel: search_music with FILTER, QUERY, NB, START, OUTPUT)
    void searchTracks(const QString& query, int limit = 25);
    void searchAlbums(const QString& query, int limit = 25);
    void searchArtists(const QString& query, int limit = 25);
    // Context-aware search for independent widgets - pass widget pointer as context
    void searchTracksWithContext(const QString& query, int limit, void* context);
    void searchAlbumsWithContext(const QString& query, int limit, void* context);

    void getUserAlbums();

    // Playlists (diezel: playlist.getList, mobile.pagePlaylist, playlist.getSongs)
    void getUserPlaylists();
    void getPlaylist(const QString& playlistId);

    // Tracks / album (diezel: song_getData, mobile.pageAlbum)
    void getTrack(const QString& trackId);
    void getAlbum(const QString& albumId);        // Returns album + songs (like diezel getAlbum)
    void getAlbumTracks(const QString& albumId);  // Legacy: just tracks

    // Lyrics
    void getLyrics(const QString& trackId);

    // User
    void getUserInfo();

    // Stream URL: diezel MediaClient get_url. If format is empty, requests best available (FLAC > MP3_320 > AAC_96 > … > MP3_128).
    void getStreamUrl(const QString& trackId, const QString& trackToken, const QString& format = QString());

    // Favorites
    void fetchFavoriteTrackIds();
    void addFavoriteTrack(const QString& trackId, const QString& contextType = QString(), const QString& contextId = QString());
    void removeFavoriteTrack(const QString& trackId, const QString& contextType = QString(), const QString& contextId = QString());
    bool isTrackFavorite(const QString& trackId) const { return m_favoriteTrackIds.contains(trackId); }

    // Listen logging
    void reportListen(const QString& trackId, int duration, const QString& format,
                     const QString& contextType = QString(), const QString& contextId = QString());

signals:
    void authenticated(const QString& username);
    void authenticationFailed(const QString& error);
    void tracksFound(QList<std::shared_ptr<Track>> tracks);
    void playlistsFound(QList<std::shared_ptr<Playlist>> playlists);
    void albumsFound(QList<std::shared_ptr<Album>> albums);
    // Search results with context tracking - void* is the requesting widget's pointer
    void searchTracksFound(QList<std::shared_ptr<Track>> tracks, void* sender);
    void searchAlbumsFound(QList<std::shared_ptr<Album>> albums, void* sender);
    void playlistReceived(std::shared_ptr<Playlist> playlist);
    void trackReceived(std::shared_ptr<Track> track);
    void albumReceived(std::shared_ptr<Album> album, QList<std::shared_ptr<Track>> songs);
    void userInfoReceived(QJsonObject userInfo);
    // format is from API (e.g. MP3_128, AAC_96, FLAC) or "MP3_128" for preview fallback
    void streamUrlReceived(const QString& trackId, const QString& url, const QString& format);
    void lyricsReceived(const QString& trackId, const QString& lyrics, const QJsonArray& syncedLyrics);
    void favoriteChanged(const QString& trackId, bool isFavorite);
    void favoriteTrackIdsLoaded();
    void error(const QString& errorMessage);
    void debugLog(const QString& message);

private slots:
    void handleNetworkReply(QNetworkReply* reply);

private:
    void ensureSid();  // Calls initializeKeys() if no SID (async)
    QNetworkReply* callGatewayMethod(const QString& method, const QJsonObject& params, bool useSid = true);
    void callWebGatewayMethod(const QString& method, const QJsonObject& params);
    QByteArray buildGatewayPostBody(const QJsonObject& params);

    std::shared_ptr<Track> parseTrack(const QJsonObject& trackJson);
    std::shared_ptr<Playlist> parsePlaylist(const QJsonObject& playlistJson);
    std::shared_ptr<Album> parseAlbum(const QJsonObject& albumJson);
    QString imageUrlForObject(const QString& type, const QString& pictureId, int width = 500, int height = 500);
    QString md5Hex(const QString& input);

    DeezerAuth* m_auth;
    QNetworkAccessManager* m_networkManager;
    int m_requestId;
    QMap<QNetworkReply*, void*> m_searchContexts;  // Track context for search requests
    QMap<QNetworkReply*, QString> m_searchFilters;  // Track FILTER type (TRACK/ALBUM/ARTIST) for each search request
    QMap<QNetworkReply*, QString> m_lyricsTrackIds;  // Track ID for lyrics requests

    static const QString GATEWAY_URL;   // https://api.deezer.com/1.0/gateway.php
    static const QString WEB_GATEWAY_URL; // For deezer.getUserData when using ARL to get user id
    static const QString IMAGE_BASE_URL;

    static QString s_mobileApiKey;
    static QString s_trackXorKey;  // 16-char key for stream decrypt (diezel TRACK_XOR_KEY), use .toLatin1() for bytes
    static QString s_licenseTokenOverride;

    QMap<QNetworkReply*, QString> m_pendingRequests;
    QSet<QString> m_favoriteTrackIds;
};

#endif // DEEZERAPI_H
