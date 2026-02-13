#ifndef LASTFMAPI_H
#define LASTFMAPI_H

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonObject>
#include <QMap>
#include <QString>

/**
 * Last.fm API client for fetching scrobble counts and user data.
 * Uses Last.fm Web Services API: http://ws.audioscrobbler.com/2.0/
 *
 * Authentication flow:
 * 1. Call getToken() -> receive token
 * 2. User authorizes in browser at http://www.last.fm/api/auth/?api_key=XXX&token=YYY
 * 3. Call getSession(token) -> receive session key
 * 4. Store session key for persistent authentication
 */
class LastFmAPI : public QObject
{
    Q_OBJECT

public:
    explicit LastFmAPI(QObject *parent = nullptr);
    ~LastFmAPI();

    // Authentication
    void setApiKey(const QString& apiKey);
    void setApiSecret(const QString& apiSecret);
    void setSessionKey(const QString& sessionKey);
    void setUsername(const QString& username);

    QString apiKey() const { return m_apiKey; }
    QString apiSecret() const { return m_apiSecret; }
    QString sessionKey() const { return m_sessionKey; }
    QString username() const { return m_username; }

    bool isAuthenticated() const;
    void getToken();  // Step 1: Get auth token
    void getSession(const QString& token);  // Step 2: Exchange token for session key
    void logout();

    // API methods
    void getTrackInfo(const QString& artist, const QString& track);
    void getAlbumInfo(const QString& artist, const QString& album);
    void getUserInfo(const QString& username);

signals:
    void tokenReceived(const QString& token);
    void authenticated(const QString& username);
    void authenticationFailed(const QString& error);
    void trackInfoReceived(const QString& trackKey, int playcount, int userPlaycount);
    void albumInfoReceived(const QString& albumKey, int playcount, int userPlaycount);
    void userInfoReceived(const QString& username, int playcount);
    void error(const QString& errorMessage);

private slots:
    void handleNetworkReply(QNetworkReply* reply);

private:
    enum RequestType {
        GetToken,
        GetSession,
        GetTrackInfo,
        GetAlbumInfo,
        GetUserInfo
    };

    void makeRequest(const QString& method, const QMap<QString, QString>& params, RequestType type);
    QString buildApiSignature(const QMap<QString, QString>& params) const;
    QString makeTrackKey(const QString& artist, const QString& track) const;
    QString makeAlbumKey(const QString& artist, const QString& album) const;

    QNetworkAccessManager* m_networkManager;
    QString m_apiKey;
    QString m_apiSecret;
    QString m_sessionKey;
    QString m_username;

    QMap<QNetworkReply*, RequestType> m_pendingRequests;
    QMap<QNetworkReply*, QString> m_requestContext;  // Store artist|track for track info requests

    static const QString API_URL;
};

#endif // LASTFMAPI_H
