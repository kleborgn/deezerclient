#include "lastfmapi.h"
#include <QUrl>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QCryptographicHash>
#include <QDebug>

const QString LastFmAPI::API_URL = "http://ws.audioscrobbler.com/2.0/";

LastFmAPI::LastFmAPI(QObject *parent)
    : QObject(parent)
    , m_networkManager(new QNetworkAccessManager(this))
{
    connect(m_networkManager, &QNetworkAccessManager::finished,
            this, &LastFmAPI::handleNetworkReply);
}

LastFmAPI::~LastFmAPI()
{
}

void LastFmAPI::setApiKey(const QString& apiKey)
{
    m_apiKey = apiKey;
}

void LastFmAPI::setApiSecret(const QString& apiSecret)
{
    m_apiSecret = apiSecret;
}

void LastFmAPI::setSessionKey(const QString& sessionKey)
{
    m_sessionKey = sessionKey;
}

void LastFmAPI::setUsername(const QString& username)
{
    m_username = username;
}

bool LastFmAPI::isAuthenticated() const
{
    return !m_sessionKey.isEmpty() && !m_apiKey.isEmpty();
}

void LastFmAPI::logout()
{
    m_sessionKey.clear();
    m_username.clear();
}

void LastFmAPI::getToken()
{
    if (m_apiKey.isEmpty()) {
        emit authenticationFailed("API key not set");
        return;
    }

    QMap<QString, QString> params;
    params["method"] = "auth.gettoken";
    params["api_key"] = m_apiKey;

    makeRequest("auth.gettoken", params, GetToken);
}

void LastFmAPI::getSession(const QString& token)
{
    if (m_apiKey.isEmpty() || m_apiSecret.isEmpty()) {
        emit authenticationFailed("API key or secret not set");
        return;
    }

    QMap<QString, QString> params;
    params["method"] = "auth.getsession";
    params["api_key"] = m_apiKey;
    params["token"] = token;

    makeRequest("auth.getsession", params, GetSession);
}

void LastFmAPI::getTrackInfo(const QString& artist, const QString& track)
{
    if (!isAuthenticated()) {
        qDebug() << "[LastFm] Not authenticated, skipping track info request";
        return;
    }

    QMap<QString, QString> params;
    params["method"] = "track.getinfo";
    params["api_key"] = m_apiKey;
    params["artist"] = artist;
    params["track"] = track;
    params["username"] = m_username;

    QString trackKey = makeTrackKey(artist, track);

    makeRequest("track.getinfo", params, GetTrackInfo);

    // Store context for later retrieval
    // We'll associate the reply with the trackKey in handleNetworkReply
}

void LastFmAPI::getAlbumInfo(const QString& artist, const QString& album)
{
    if (!isAuthenticated()) {
        qDebug() << "[LastFm] Not authenticated, skipping album info request";
        return;
    }

    QMap<QString, QString> params;
    params["method"] = "album.getinfo";
    params["api_key"] = m_apiKey;
    params["artist"] = artist;
    params["album"] = album;
    params["username"] = m_username;

    QString albumKey = makeAlbumKey(artist, album);

    makeRequest("album.getinfo", params, GetAlbumInfo);
}

void LastFmAPI::getUserInfo(const QString& username)
{
    if (m_apiKey.isEmpty()) {
        emit error("API key not set");
        return;
    }

    QMap<QString, QString> params;
    params["method"] = "user.getinfo";
    params["api_key"] = m_apiKey;
    params["user"] = username;

    makeRequest("user.getinfo", params, GetUserInfo);
}

void LastFmAPI::makeRequest(const QString& method, const QMap<QString, QString>& params, RequestType type)
{
    QUrl url(API_URL);
    QUrlQuery query;

    // Add all parameters to query
    for (auto it = params.constBegin(); it != params.constEnd(); ++it) {
        query.addQueryItem(it.key(), it.value());
    }

    // Add format
    query.addQueryItem("format", "json");

    // Add API signature for authenticated methods
    if (type == GetSession || type == GetTrackInfo || type == GetAlbumInfo) {
        QString signature = buildApiSignature(params);
        query.addQueryItem("api_sig", signature);
    }

    url.setQuery(query);

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, "DeezerClient-LastFm/1.0");

    QNetworkReply* reply = m_networkManager->get(request);
    m_pendingRequests[reply] = type;

    // Store context for track/album requests
    if (type == GetTrackInfo) {
        QString trackKey = makeTrackKey(params["artist"], params["track"]);
        m_requestContext[reply] = trackKey;
    } else if (type == GetAlbumInfo) {
        QString albumKey = makeAlbumKey(params["artist"], params["album"]);
        m_requestContext[reply] = albumKey;
    }

    qDebug() << "[LastFm] Request:" << method << url.toString();
}

QString LastFmAPI::buildApiSignature(const QMap<QString, QString>& params) const
{
    // API signature: MD5(sorted params concatenated + secret)
    // Example: api_key=XXXmethod=auth.getsessiontoken=YYY<secret>

    QStringList sortedKeys = params.keys();
    sortedKeys.sort();

    QString signatureString;
    for (const QString& key : sortedKeys) {
        // Skip 'format' and 'callback' parameters
        if (key == "format" || key == "callback") {
            continue;
        }
        signatureString += key + params[key];
    }
    signatureString += m_apiSecret;

    QByteArray hash = QCryptographicHash::hash(signatureString.toUtf8(), QCryptographicHash::Md5);
    return hash.toHex();
}

QString LastFmAPI::makeTrackKey(const QString& artist, const QString& track) const
{
    return (artist.trimmed() + "|" + track.trimmed()).toLower();
}

QString LastFmAPI::makeAlbumKey(const QString& artist, const QString& album) const
{
    return (artist.trimmed() + "|" + album.trimmed()).toLower();
}

void LastFmAPI::handleNetworkReply(QNetworkReply* reply)
{
    reply->deleteLater();

    RequestType requestType = m_pendingRequests.value(reply, GetToken);
    m_pendingRequests.remove(reply);

    QString context = m_requestContext.value(reply, QString());
    m_requestContext.remove(reply);

    if (reply->error() != QNetworkReply::NoError) {
        QString errorMsg = QString("Network error: %1").arg(reply->errorString());
        qDebug() << "[LastFm]" << errorMsg;

        if (requestType == GetToken || requestType == GetSession) {
            emit authenticationFailed(errorMsg);
        } else {
            emit error(errorMsg);
        }
        return;
    }

    QByteArray data = reply->readAll();
    QJsonDocument doc = QJsonDocument::fromJson(data);

    if (doc.isNull()) {
        emit error("Invalid JSON response from Last.fm");
        return;
    }

    QJsonObject root = doc.object();

    // Check for Last.fm API error
    if (root.contains("error")) {
        int errorCode = root["error"].toInt();
        QString errorMsg = root["message"].toString();
        qDebug() << "[LastFm] API error:" << errorCode << errorMsg;

        if (requestType == GetToken || requestType == GetSession) {
            emit authenticationFailed(errorMsg);
        } else {
            emit error(QString("Last.fm error %1: %2").arg(errorCode).arg(errorMsg));
        }
        return;
    }

    // Parse response based on request type
    switch (requestType) {
        case GetToken: {
            QString token = root["token"].toString();
            if (!token.isEmpty()) {
                qDebug() << "[LastFm] Token received:" << token;
                emit tokenReceived(token);
            } else {
                emit authenticationFailed("No token in response");
            }
            break;
        }

        case GetSession: {
            QJsonObject session = root["session"].toObject();
            QString sessionKey = session["key"].toString();
            QString username = session["name"].toString();

            if (!sessionKey.isEmpty() && !username.isEmpty()) {
                m_sessionKey = sessionKey;
                m_username = username;
                qDebug() << "[LastFm] Authenticated as:" << username;
                emit authenticated(username);
            } else {
                emit authenticationFailed("Invalid session data");
            }
            break;
        }

        case GetTrackInfo: {
            QJsonObject track = root["track"].toObject();

            int playcount = track["playcount"].toString().toInt();
            int userPlaycount = track["userplaycount"].toString().toInt();

            qDebug() << "[LastFm] Track info:" << context << "playcount=" << playcount << "userplaycount=" << userPlaycount;
            emit trackInfoReceived(context, playcount, userPlaycount);
            break;
        }

        case GetAlbumInfo: {
            QJsonObject album = root["album"].toObject();

            int playcount = album["playcount"].toString().toInt();
            int userPlaycount = album["userplaycount"].toString().toInt();

            qDebug() << "[LastFm] Album info:" << context << "playcount=" << playcount << "userplaycount=" << userPlaycount;
            emit albumInfoReceived(context, playcount, userPlaycount);
            break;
        }

        case GetUserInfo: {
            QJsonObject user = root["user"].toObject();
            QString username = user["name"].toString();
            int playcount = user["playcount"].toString().toInt();

            qDebug() << "[LastFm] User info:" << username << "playcount=" << playcount;
            emit userInfoReceived(username, playcount);
            break;
        }
    }
}
