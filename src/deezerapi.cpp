#include "deezerapi.h"
#include <QNetworkRequest>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QCryptographicHash>
#include <QDebug>
#include <QRandomGenerator>
#include <QByteArray>

#ifdef DEEZER_OPENSSL
#include <openssl/evp.h>
#include <openssl/aes.h>
#include <openssl/rand.h>
#include <cstring>
#endif
#include "blowfish_jukebox.h"
#include "secrets.h"

const QString DeezerAPI::GATEWAY_URL = "https://api.deezer.com/1.0/gateway.php";
const QString DeezerAPI::WEB_GATEWAY_URL = "https://www.deezer.com/ajax/gw-light.php";
const QString DeezerAPI::IMAGE_BASE_URL = "https://e-cdns-images.dzcdn.net/images";

QString DeezerAPI::s_mobileApiKey = DEEZER_MOBILE_API_KEY;
QString DeezerAPI::s_trackXorKey = DEEZER_TRACK_XOR_KEY;
QString DeezerAPI::s_licenseTokenOverride = "";

// Diezel-style User-Agent (Android)
static const char* USER_AGENT = "Deezer/6.1.22.49 (Android; 9; Tablet; us) innotek GmbH VirtualBox";

DeezerAPI::DeezerAPI(QObject *parent)
    : QObject(parent)
    , m_auth(new DeezerAuth(this))
    , m_networkManager(new QNetworkAccessManager(this))
    , m_requestId(0)
{
    connect(m_networkManager, &QNetworkAccessManager::finished,
            this, &DeezerAPI::handleNetworkReply);

    // Forward authentication signals
    connect(m_auth, &DeezerAuth::authenticated, this, &DeezerAPI::authenticated);
    connect(m_auth, &DeezerAuth::authenticationFailed, this, &DeezerAPI::authenticationFailed);
    connect(m_auth, &DeezerAuth::debugLog, this, &DeezerAPI::debugLog);
}

DeezerAPI::~DeezerAPI()
{
}

void DeezerAPI::setApiKey(const QString& apiKey)
{
    s_mobileApiKey = apiKey;
}

void DeezerAPI::setMobileGwKey(const QString& key)
{
    DeezerAuth::setMobileGwKey(key);
}

void DeezerAPI::setTrackXorKey(const QByteArray& key)
{
    s_trackXorKey = QString::fromLatin1(key.size() >= 16 ? key.left(16) : key);
}

void DeezerAPI::setTrackXorKey(const QString& key)
{
    QString k = key.trimmed();
    k.remove(QChar(' '));
    QByteArray keyBytes = QByteArray::fromHex(k.toLatin1());
    if (keyBytes.size() >= 16)
        s_trackXorKey = QString::fromLatin1(keyBytes.left(16));
    else
        s_trackXorKey = k.left(16);
}

void DeezerAPI::setLicenseToken(const QString& token)
{
    s_licenseTokenOverride = token;
}

void DeezerAPI::ensureSid()
{
    if (!m_auth->sid().isEmpty())
        return;
    if (s_mobileApiKey.isEmpty()) {
        emit error("MOBILE_API_KEY not set. Call DeezerAPI::setApiKey().");
        return;
    }
    // The auth class will handle initialization
}

QNetworkReply* DeezerAPI::callGatewayMethod(const QString& method, const QJsonObject& params, bool useSid)
{
    if (s_mobileApiKey.isEmpty()) {
        emit error("MOBILE_API_KEY not set. Call DeezerAPI::setApiKey().");
        return nullptr;
    }
    QString sid = m_auth->sid();
    if (useSid && sid.isEmpty()) {
        emit error("Not logged in. Please log in first.");
        return nullptr;
    }

    QUrl url(GATEWAY_URL);
    QUrlQuery query;
    query.addQueryItem("api_key", s_mobileApiKey);
    query.addQueryItem("output", "3");
    query.addQueryItem("input", "3");
    query.addQueryItem("method", method);
    if (useSid && !sid.isEmpty())
        query.addQueryItem("sid", sid);
    url.setQuery(query);

    QByteArray postData = buildGatewayPostBody(params);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("User-Agent", USER_AGENT);

    QNetworkReply* reply = m_networkManager->post(request, postData);
    m_pendingRequests[reply] = method;
    qDebug() << "[DeezerAPI] POST" << method << "sid=" << (sid.isEmpty() ? "none" : "set");
    emit debugLog(QString("[%1] Request sent (POST)").arg(method));
    
    return reply;
}

void DeezerAPI::callWebGatewayMethod(const QString& method, const QJsonObject& params)
{
    QUrl url(WEB_GATEWAY_URL);
    QUrlQuery query;
    query.addQueryItem("api_version", "1.0");
    query.addQueryItem("api_token", m_auth->checkForm().isEmpty() ? "null" : m_auth->checkForm());
    query.addQueryItem("input", "3");
    query.addQueryItem("output", "3");
    query.addQueryItem("cid", QString::number(QRandomGenerator::global()->generate()));
    query.addQueryItem("method", method);
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("User-Agent", USER_AGENT);
    request.setRawHeader("X-Requested-With", "XMLHttpRequest");

    QString cookies = m_auth->buildCookieString();
    if (!cookies.isEmpty())
        request.setRawHeader("Cookie", cookies.toUtf8());

    QByteArray postData = QJsonDocument(params).toJson(QJsonDocument::Compact);
    QNetworkReply* reply = m_networkManager->post(request, postData);
    m_pendingRequests[reply] = method;

    QString logMsg = QString("[%1] Request sent (Web Gateway POST)")
        .arg(method);
    emit debugLog(logMsg);
}

QByteArray DeezerAPI::buildGatewayPostBody(const QJsonObject& params)
{
    // Diezel: POST body = params object only (no JSON-RPC id/method/jsonrpc wrapper)
    return QJsonDocument(params).toJson(QJsonDocument::Compact);
}

void DeezerAPI::signInWithEmail(const QString& email, const QString& password)
{
    m_auth->signInWithEmail(email, password);
}

void DeezerAPI::signInWithArl(const QString& arl)
{
    m_auth->signInWithArl(arl);
}

bool DeezerAPI::isAuthenticated() const
{
    return m_auth->isAuthenticated();
}

QString DeezerAPI::sid() const
{
    return m_auth->sid();
}

QString DeezerAPI::arl() const
{
    return m_auth->arl();
}

QString DeezerAPI::checkForm() const
{
    return m_auth->checkForm();
}

void DeezerAPI::logout()
{
    m_auth->logout();
}

void DeezerAPI::searchTracks(const QString& query, int limit)
{
    searchTracksWithContext(query, limit, nullptr);
}

void DeezerAPI::searchAlbums(const QString& query, int limit)
{
    searchAlbumsWithContext(query, limit, nullptr);
}

void DeezerAPI::searchTracksWithContext(const QString& query, int limit, void* context)
{
    QJsonObject params;
    params["QUERY"] = query;
    params["FILTER"] = "TRACK";
    params["NB"] = QString::number(limit);
    params["START"] = 0;
    params["OUTPUT"] = "TRACK";
    // Call the API with the standard method name and get the reply
    QNetworkReply* reply = callGatewayMethod("search_music", params);

    // Store the context and filter type with the reply
    if (reply) {
        if (context) {
            m_searchContexts[reply] = context;
        }
        m_searchFilters[reply] = "TRACK";
    }
}

void DeezerAPI::searchAlbumsWithContext(const QString& query, int limit, void* context)
{
    QJsonObject params;
    params["QUERY"] = query;
    params["FILTER"] = "ALBUM";
    params["NB"] = QString::number(limit);
    params["START"] = 0;
    params["OUTPUT"] = "ALBUM";
    // Call the API with the standard method name and get the reply
    QNetworkReply* reply = callGatewayMethod("search_music", params);

    // Store the context and filter type with the reply
    if (reply) {
        if (context) {
            m_searchContexts[reply] = context;
        }
        m_searchFilters[reply] = "ALBUM";
    }
}

void DeezerAPI::searchArtists(const QString& query, int limit)
{
    QJsonObject params;
    params["QUERY"] = query;
    params["FILTER"] = "ARTIST";
    params["NB"] = QString::number(limit);
    params["START"] = 0;
    params["OUTPUT"] = "ARTIST";
    QNetworkReply* reply = callGatewayMethod("search_music", params);

    // Store the filter type with the reply
    if (reply) {
        m_searchFilters[reply] = "ARTIST";
    }
}

void DeezerAPI::getUserPlaylists()
{
    if (!m_auth->isAuthenticated() || m_auth->userId().isEmpty()) {
        emit error("Not authenticated");
        return;
    }
    QJsonObject params;
    params["user_id"] = m_auth->userId();
    params["nb"] = "1000";
    params["ARRAY_DEFAULT"] = QJsonArray::fromStringList({
        "PLAYLIST_ID", "TITLE", "PICTURE_TYPE", "PLAYLIST_PICTURE",
        "STATUS", "TYPE", "DATE_CREATE", "DATE_ADD", "DATE_MOD", "NB_SONG"
    });
    emit debugLog(QString("getUserPlaylists: sending user_id=%1").arg(m_auth->userId()));
    callGatewayMethod("playlist.getList", params);
}

void DeezerAPI::getUserAlbums()
{
    if (!m_auth->isAuthenticated() || m_auth->userId().isEmpty()) {
        emit error("Not authenticated");
        return;
    }
    QJsonObject params;
    params["user_id"] = m_auth->userId();
    params["NB"] = "10000";
    emit debugLog(QString("getUserAlbums: sending user_id=%1 via album.getFavorites").arg(m_auth->userId()));
    callGatewayMethod("album.getFavorites", params);
}

void DeezerAPI::reportListen(const QString& trackId, int duration, const QString& format,
                            const QString& contextType, const QString& contextId)
{
    if (m_auth->sid().isEmpty()) return;

    QJsonObject media;
    media["id"] = trackId;
    media["type"] = "song";
    media["format"] = format.isEmpty() ? "MP3_128" : format;

    QJsonObject stat;
    stat["pause"] = 0;
    stat["seek"] = 0;
    stat["sync"] = 0;

    QJsonObject p;
    p["media"] = media;
    p["type"] = 0; // Matching web app example
    p["stat"] = stat;
    p["lt"] = duration;
    p["ts_listen"] = QDateTime::currentSecsSinceEpoch();
    p["timestamp"] = QDateTime::currentSecsSinceEpoch();

    // Add context (album or playlist source) if provided
    // contextType examples: "album_page" for albums, "profile_playlists" for playlists
    if (!contextType.isEmpty() && !contextId.isEmpty()) {
        QJsonObject ctxt;
        ctxt["t"] = contextType;
        ctxt["id"] = contextId;
        p["ctxt"] = ctxt;
    }

    QJsonObject root;
    root["params"] = p;

    callGatewayMethod("log.listen", root);
}

void DeezerAPI::getPlaylist(const QString& playlistId)
{
    QJsonObject params;
    params["playlist_id"] = playlistId;
    params["nb"] = 2000;
    params["start"] = 0;
    callWebGatewayMethod("deezer.pagePlaylist", params);
}

void DeezerAPI::getTrack(const QString& trackId)
{
    QJsonObject params;
    // Convert trackId to number - API expects numeric SNG_ID
    bool ok;
    qint64 id = trackId.toLongLong(&ok);
    if (ok) {
        params["SNG_ID"] = id;
    } else {
        params["SNG_ID"] = trackId;  // Fallback to string if conversion fails
    }
    callGatewayMethod("song_getData", params);
}

void DeezerAPI::getAlbum(const QString& albumId)
{
    QJsonObject params;
    params["alb_id"] = albumId;
    params["user_id"] = m_auth->userId().isEmpty() ? "0" : m_auth->userId();
    params["lang"] = "en";
    params["header"] = true;
    params["tab"] = 0;
    callGatewayMethod("mobile.pageAlbum", params);
}

void DeezerAPI::getAlbumTracks(const QString& albumId)
{
    getAlbum(albumId);
}

void DeezerAPI::fetchFavoriteTrackIds()
{
    QJsonObject params;
    params["USER_ID"] = m_auth->userId();
    params["nb"] = 10000;
    params["start"] = 0;
    callWebGatewayMethod("favorite_song.getList", params);
}

void DeezerAPI::addFavoriteTrack(const QString& trackId, const QString& contextType, const QString& contextId)
{
    QJsonObject params;
    params["SNG_ID"] = trackId.toLongLong();
    QJsonArray ids;
    ids.append(trackId);
    params["IDS"] = ids;
    if (!contextType.isEmpty() && !contextId.isEmpty()) {
        QJsonObject ctxt;
        ctxt["id"] = contextId;
        ctxt["t"] = contextType;
        params["CTXT"] = ctxt;
    }
    m_favoriteTrackIds.insert(trackId);
    callWebGatewayMethod("favorite_song.add", params);
}

void DeezerAPI::removeFavoriteTrack(const QString& trackId, const QString& contextType, const QString& contextId)
{
    QJsonObject params;
    params["SNG_ID"] = trackId.toLongLong();
    QJsonArray ids;
    ids.append(trackId);
    params["IDS"] = ids;
    if (!contextType.isEmpty() && !contextId.isEmpty()) {
        QJsonObject ctxt;
        ctxt["id"] = contextId;
        ctxt["t"] = contextType;
        params["CTXT"] = ctxt;
    }
    m_favoriteTrackIds.remove(trackId);
    callWebGatewayMethod("favorite_song.remove", params);
}

void DeezerAPI::getLyrics(const QString& trackId)
{
    QJsonObject params;
    // Convert trackId to number - API expects numeric SNG_ID
    bool ok;
    qint64 id = trackId.toLongLong(&ok);
    if (ok) {
        params["SNG_ID"] = id;
    } else {
        params["SNG_ID"] = trackId;  // Fallback to string if conversion fails
    }
    QNetworkReply* reply = callGatewayMethod("song.getLyrics", params);
    if (reply) {
        m_lyricsTrackIds[reply] = trackId;  // Store track ID for later retrieval
    }
}

void DeezerAPI::getUserInfo()
{
    if (!m_auth->isAuthenticated()) {
        emit error("Not authenticated");
        return;
    }
    // Diezel doesn't have a separate getUserInfo; we already have user from login.
    // Could call a method that returns user or use stored data.
    QJsonObject user;
    user["USER_ID"] = m_auth->userId().toLongLong();
    user["BLOG_NAME"] = m_auth->username();
    emit userInfoReceived(user);
}

// Preferred stream formats: FLAC first (BASSFLAC available), then MP3 fallbacks.
static const QStringList STREAM_FORMAT_PREFERENCE = {
    QStringLiteral("FLAC"),
    QStringLiteral("MP3_320"),
    QStringLiteral("MP3_256"),
    QStringLiteral("MP3_192"),
    QStringLiteral("MP3_128"),
    QStringLiteral("AAC_96"),
};

// Build media/get_url request body exactly like diezel MediaClient.getSongStreams
// https://github.com/svbnet/diezel/blob/master/lib/clients/media-client.js
void DeezerAPI::getStreamUrl(const QString& trackId, const QString& trackToken, const QString& format)
{
    if (trackToken.isEmpty()) {
        QString preview = QString("https://cdns-preview-e.dzcdn.net/stream/c-%1-1.mp3").arg(trackId);
        emit streamUrlReceived(trackId, preview, QStringLiteral("MP3_128")); // preview is 30s MP3
        return;
    }
    QString mediaUrl = m_auth->mediaUrl();
    QString licenseToken = m_auth->licenseToken();
    if (mediaUrl.isEmpty() || (licenseToken.isEmpty() && s_licenseTokenOverride.isEmpty())) {
        if (mediaUrl.isEmpty())
            emit debugLog("getStreamUrl: URL_MEDIA is empty (from mobile_auth). Log in again or check API.");
        if (licenseToken.isEmpty() && s_licenseTokenOverride.isEmpty())
            emit debugLog("getStreamUrl: license_token is empty (decrypt PREMIUM.RANDOM after login). Using preview.");
        QString preview = QString("https://cdns-preview-e.dzcdn.net/stream/c-%1-1.mp3").arg(trackId);
        emit streamUrlReceived(trackId, preview, QStringLiteral("MP3_128"));
        return;
    }
    QString token = s_licenseTokenOverride.isEmpty() ? licenseToken : s_licenseTokenOverride;
    // Body: { license_token, track_tokens, media: [ { type, formats: [{ cipher, format }] }, ... ] }
    // Send one media object per format so API returns one media[] entry per format (we then pick best).
    QJsonObject body;
    body["license_token"] = token;
    body["track_tokens"] = QJsonArray::fromStringList({ trackToken });
    QJsonArray mediaArr;
    QStringList formatsToRequest = format.isEmpty() ? STREAM_FORMAT_PREFERENCE : QStringList{ format };
    for (const QString& f : formatsToRequest) {
        QJsonArray formatsJson;
        QJsonObject formatItem;
        formatItem["cipher"] = "BF_CBC_STRIPE";
        formatItem["format"] = f;
        formatsJson.append(formatItem);
        QJsonObject mediaItem;
        mediaItem["type"] = "FULL";
        mediaItem["formats"] = formatsJson;
        mediaArr.append(mediaItem);
    }
    body["media"] = mediaArr;

    QUrl url(mediaUrl + "/v1/get_url");
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("User-Agent", USER_AGENT);

    QByteArray postData = QJsonDocument(body).toJson(QJsonDocument::Compact);
    QNetworkReply* reply = m_networkManager->post(request, postData);
    m_pendingRequests[reply] = "get_url:" + trackId;
    emit debugLog(QString("getStreamUrl: POST %1/v1/get_url for track %2 (formats: %3)").arg(mediaUrl, trackId, formatsToRequest.join(',')));
}

void DeezerAPI::handleNetworkReply(QNetworkReply* reply)
{
    reply->deleteLater();
    QString method = m_pendingRequests.value(reply, "");
    m_pendingRequests.remove(reply);

    if (reply->error() != QNetworkReply::NoError) {
        emit error(reply->errorString());
        return;
    }

    QByteArray data = reply->readAll();
    QString rawResponse = QString::fromUtf8(data);

    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject()) {
        emit debugLog(QString("[%1] Invalid JSON. Raw: %2").arg(method, rawResponse.left(500)));
        emit error("Invalid JSON response");
        return;
    }
    QJsonObject obj = doc.object();

    // Only treat as error when "error" is a non-empty object, non-empty array, or non-empty string (API often returns error: null, error: {}, or error: [] on success)
    if (obj.contains("error")) {
        QJsonValue errVal = obj["error"];
        bool isError = false;
        QString msg;
        if (errVal.isNull() || errVal.isUndefined()) {
            isError = false;
        } else if (errVal.isArray()) {
            if (!errVal.toArray().isEmpty()) isError = true;
        } else if (errVal.isObject()) {
            QJsonObject err = errVal.toObject();
            if (!err.isEmpty()) {
                isError = true;
                for (auto it = err.begin(); it != err.end(); ++it) {
                    QJsonValue v = it.value();
                    if (v.isString()) msg += QString("%1: %2\n").arg(it.key(), v.toString());
                    else if (v.isDouble()) msg += QString("%1: %2\n").arg(it.key()).arg(v.toDouble());
                    else if (v.isArray()) msg += QString("%1: (array)\n").arg(it.key());
                    else msg += QString("%1: %2\n").arg(it.key(), QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact));
                }
            }
        } else if (errVal.isString()) {
            msg = errVal.toString().trimmed();
            if (!msg.isEmpty()) isError = true;
        } else {
            isError = true;
            msg = "API returned error (see Help → View debug log for raw response)";
        }
        if (isError) {
            if (msg.isEmpty()) msg = "API error (see Help → View debug log for raw response)";
            QString logLine = QString("[%1] API error: %2\nRaw response: %3")
                                 .arg(method, msg, rawResponse.left(2000));
            emit debugLog(logLine);
            if (method == "mobile_userAuth" || method == "deezer.getUserData" || method.startsWith("mobile_user"))
                emit authenticationFailed(msg);
            else
                emit error(msg);
            return;
        }
    }

    QJsonValue resultsVal = obj["results"];
    if (resultsVal.isUndefined())
        resultsVal = obj["result"];
    if (resultsVal.isUndefined() && obj.contains("data"))
        resultsVal = obj["data"];
    if (resultsVal.isUndefined() && obj.contains("body"))
        resultsVal = obj["body"];
    if (resultsVal.isUndefined() && method != "mobile_auth") {
        emit error("No results in response");
        return;
    }
    if (resultsVal.isUndefined() && method == "mobile_auth") {
        resultsVal = QJsonValue(obj);
    }
    if (resultsVal.isUndefined()) {
        emit error("No results in response");
        return;
    }

    if (method == "log.listen") {
        return;
    }

    if (method == "favorite_song.add" || method == "favorite_song.remove") {
        emit debugLog(QString("[%1] Success").arg(method));
        return;
    }

    if (method == "favorite_song.getList") {
        m_favoriteTrackIds.clear();
        QJsonObject results = resultsVal.toObject();
        QJsonArray dataArray = results["data"].toArray();
        for (const QJsonValue& v : dataArray) {
            QJsonObject song = v.toObject();
            QString sngId = song.contains("SNG_ID")
                ? QString::number(song["SNG_ID"].toVariant().toLongLong())
                : song["id"].toVariant().toString();
            if (!sngId.isEmpty())
                m_favoriteTrackIds.insert(sngId);
        }
        emit debugLog(QString("[favorite_song.getList] Loaded %1 favorite track IDs").arg(m_favoriteTrackIds.size()));
        emit favoriteTrackIdsLoaded();
        return;
    }

    if (method == "song_getData") {
        QJsonObject results = resultsVal.isObject() ? resultsVal.toObject() : QJsonObject();
        emit trackReceived(parseTrack(results));
        return;
    }

    if (method == "song.getLyrics") {
        QJsonObject results = resultsVal.isObject() ? resultsVal.toObject() : QJsonObject();

        // Retrieve track ID from stored map
        QString trackId = m_lyricsTrackIds.value(reply, QString());
        m_lyricsTrackIds.remove(reply);  // Clean up

        // Extract plain text lyrics
        QString lyrics;
        if (results.contains("LYRICS")) {
            lyrics = results["LYRICS"].toString();
        } else if (results.contains("lyrics")) {
            lyrics = results["lyrics"].toString();
        } else if (results.contains("LYRICS_TEXT")) {
            lyrics = results["LYRICS_TEXT"].toString();
        }

        // Extract synced lyrics with timestamps
        QJsonArray syncedLyrics;
        if (results.contains("LYRICS_SYNC_JSON")) {
            syncedLyrics = results["LYRICS_SYNC_JSON"].toArray();
        } else if (results.contains("syncedLyrics")) {
            syncedLyrics = results["syncedLyrics"].toArray();
        }

        emit debugLog(QString("[song.getLyrics] Track %1: lyrics=%2 chars, synced=%3 lines")
                      .arg(trackId)
                      .arg(lyrics.length())
                      .arg(syncedLyrics.size()));

        // Debug: Log first synced lyric line structure if available
        if (!syncedLyrics.isEmpty() && syncedLyrics.first().isObject()) {
            QJsonObject firstLine = syncedLyrics.first().toObject();
            QStringList keys = firstLine.keys();
            emit debugLog(QString("[song.getLyrics] First line fields: %1").arg(keys.join(", ")));
            emit debugLog(QString("[song.getLyrics] First line values: %1")
                         .arg(QJsonDocument(firstLine).toJson(QJsonDocument::Compact)));
        }

        emit lyricsReceived(trackId, lyrics, syncedLyrics);
        return;
    }

    if (method == "deezer.pageProfile") {
        QJsonObject results = resultsVal.toObject();
        QJsonArray sections = results["TAB"].toObject()["sections"].toArray();
        
        QList<std::shared_ptr<Album>> albums;
        QList<std::shared_ptr<Playlist>> playlists;
        
        for (const QJsonValue& sectionVal : sections) {
            QJsonObject section = sectionVal.toObject();
            if (section["target"].toString() == "ALBUMS") {
                QJsonArray data = section["data"].toArray();
                for (const QJsonValue& v : data)
                    albums.append(parseAlbum(v.toObject()));
            } else if (section["target"].toString() == "PLAYLISTS") {
                QJsonArray data = section["data"].toArray();
                for (const QJsonValue& v : data)
                    playlists.append(parsePlaylist(v.toObject()));
            }
        }
        
        if (!albums.isEmpty()) {
            emit debugLog(QString("[deezer.pageProfile] Parsed %1 albums").arg(albums.size()));
            emit albumsFound(albums);
        }
        if (!playlists.isEmpty()) {
            emit debugLog(QString("[deezer.pageProfile] Parsed %1 playlists").arg(playlists.size()));
            emit playlistsFound(playlists);
        }
        return;
    }

    if (method == "mobile.pageAlbum") {
        QJsonObject results = resultsVal.toObject();
        QJsonObject albumData = results["DATA"].toObject();
        auto album = parseAlbum(albumData);
        QJsonArray songsArray = results.contains("SONGS") ? results["SONGS"].toObject()["data"].toArray() : QJsonArray();
        QList<std::shared_ptr<Track>> tracks;
        for (const QJsonValue& v : songsArray)
            tracks.append(parseTrack(v.toObject()));
        emit albumReceived(album, tracks);
        emit tracksFound(tracks);
        return;
    }

    if (method == "deezer.pagePlaylist") {
        QJsonObject results = resultsVal.toObject();
        QJsonObject playlistData = results["DATA"].toObject();
        auto playlist = parsePlaylist(playlistData);
        
        QJsonArray songsArray;
        if (results.contains("SONGS") && results["SONGS"].isObject()) {
            songsArray = results["SONGS"].toObject()["data"].toArray();
        } else if (playlistData.contains("SONGS") && playlistData["SONGS"].isObject()) {
            songsArray = playlistData["SONGS"].toObject()["data"].toArray();
        } else if (results.contains("songs") && results["songs"].isObject()) {
            songsArray = results["songs"].toObject()["data"].toArray();
        } else if (results.contains("data")) {
            songsArray = results["data"].toArray();
        } else if (results.contains("TAB") && results["TAB"].isObject()) {
            QJsonArray sections = results["TAB"].toObject()["sections"].toArray();
            for (const QJsonValue& sectionVal : sections) {
                QJsonObject s = sectionVal.toObject();
                if (s["target"].toString() == "SONGS" || s["target"].toString() == "tracks") {
                    songsArray = s["data"].toArray();
                    break;
                }
            }
        }

        if (songsArray.isEmpty()) {
            emit debugLog(QString("[deezer.pagePlaylist] No songs found. Results keys: %1, DATA keys: %2")
                .arg(results.keys().join(", "), playlistData.keys().join(", ")));
        }

        for (const QJsonValue& v : songsArray)
            playlist->addTrack(parseTrack(v.toObject()));
            
        emit playlistReceived(playlist);
        return;
    }

    if (method == "playlist.getSongs") {
        QJsonObject results = resultsVal.toObject();
        auto playlist = parsePlaylist(results); // Fallback: try to parse metadata from top level
        QJsonArray songsArr;
        if (results.contains("data")) {
            songsArr = results["data"].toArray();
        } else if (results.contains("DATA")) {
            songsArr = results["DATA"].toArray();
        }
        for (const QJsonValue& v : songsArr)
            playlist->addTrack(parseTrack(v.toObject()));
        emit debugLog(QString("[playlist.getSongs] Loaded %1 tracks").arg(playlist->tracks().size()));
        emit playlistReceived(playlist);
        return;
    }

    if (method == "playlist.getList" || method == "album.getList" || method == "album.getUserList" || method == "album.getFavorites") {
        QJsonArray data;
        if (resultsVal.isArray()) {
            data = resultsVal.toArray();
        } else {
            QJsonObject results = resultsVal.toObject();
            data = results["data"].toArray();
            if (data.isEmpty() && results["DATA"].isObject())
                data = results["DATA"].toObject()["data"].toArray();
            if (data.isEmpty()) data = results["DATA"].toArray();
            if (data.isEmpty()) data = results["items"].toArray();
            if (data.isEmpty()) data = results["playlists"].toArray();
            if (data.isEmpty()) data = results["albums"].toArray();
            if (data.isEmpty()) {
                for (const QString& k : results.keys()) {
                    if (results[k].isArray()) {
                        data = results[k].toArray();
                        break;
                    }
                    if (results[k].isObject() && results[k].toObject().contains("data")) {
                        data = results[k].toObject()["data"].toArray();
                        if (!data.isEmpty()) break;
                    }
                }
            }
            if (data.isEmpty()) {
                emit debugLog(QString("[%1] No array found. Results keys: %2. Sample: %3")
                    .arg(method, results.keys().join(", "), rawResponse.left(800)));
            }
        }
        
        if (method == "playlist.getList") {
            QList<std::shared_ptr<Playlist>> playlists;
            for (const QJsonValue& v : data) {
                if (v.isObject())
                    playlists.append(parsePlaylist(v.toObject()));
            }
            emit debugLog(QString("[playlist.getList] Parsed %1 playlists").arg(playlists.size()));
            emit playlistsFound(playlists);
        } else {
            emit debugLog(QString("[%1] Found %2 album items").arg(method).arg(data.size()));
            QList<std::shared_ptr<Album>> albums;
            for (const QJsonValue& v : data) {
                if (v.isObject())
                    albums.append(parseAlbum(v.toObject()));
            }
            emit debugLog(QString("[%1] Parsed %2 albums").arg(method).arg(albums.size()));
            emit albumsFound(albums);
        }
        return;
    }

    if (method == "search_music" || method.startsWith("search_music_")) {
        QJsonArray data;
        QJsonObject results = resultsVal.isObject() ? resultsVal.toObject() : QJsonObject();
        
        // Check if this reply has a context (for search requests from widgets)
        void* context = nullptr;
        if (m_searchContexts.contains(reply)) {
            context = m_searchContexts.take(reply);
            if (context) {
                emit debugLog(QString("[DeezerAPI] Context-aware search result for context %1").arg(reinterpret_cast<quintptr>(context)));
            }
        }

        // Check what type of search this was (TRACK, ALBUM, or ARTIST)
        QString requestedFilter;
        if (m_searchFilters.contains(reply)) {
            requestedFilter = m_searchFilters.take(reply);
            emit debugLog(QString("[search_music] Request was for FILTER=%1").arg(requestedFilter));
        }
        
        // Debug: Show what keys are in the results
        emit debugLog(QString("[search_music] Results keys: %1").arg(results.keys().join(", ")));

        if (results.contains("ALBUM")) {
            data = results["ALBUM"].toObject()["data"].toArray();
        } else if (results.contains("TRACK")) {
            data = results["TRACK"].toObject()["data"].toArray();
            emit debugLog(QString("[search_music] Using TRACK section, found %1 items").arg(data.size()));
        } else if (results.contains("data") && results["data"].isArray()) {
            // Direct data array at top level (common for search results)
            data = results["data"].toArray();
            emit debugLog(QString("[search_music] Using direct data array, found %1 items").arg(data.size()));
        } else {
            // General fallback
            for (const QString& k : results.keys()) {
                if (results[k].isObject() && results[k].toObject().contains("data")) {
                    data = results[k].toObject()["data"].toArray();
                    emit debugLog(QString("[search_music] Using %1 section, found %2 items").arg(k).arg(data.size()));
                    break;
                }
            }
        }

        if (data.isEmpty() && results.contains("data"))
            data = results["data"].toArray();

        // Skip processing if this was an ARTIST request (we don't have artist results handling)
        if (requestedFilter == "ARTIST") {
            emit debugLog("[search_music] Skipping ARTIST results");
            return;
        }

        // Only process ALBUM results if this was an ALBUM request
        if (requestedFilter == "ALBUM") {
            QList<std::shared_ptr<Album>> albums;
            for (const QJsonValue& v : data) {
                if (v.isObject())
                    albums.append(parseAlbum(v.toObject()));
            }
            emit debugLog(QString("[search_music] Returning %1 albums").arg(albums.size()));
            // Only emit generic albumsFound for non-context searches (legacy compatibility)
            if (!context) {
                emit albumsFound(albums);
            }
            // Always emit context-aware signal
            emit searchAlbumsFound(albums, context);
        } else if (requestedFilter == "TRACK" || requestedFilter.isEmpty()) {
            // Only process TRACK results if this was a TRACK request or unknown type
            QList<std::shared_ptr<Track>> tracks;
            int skippedCount = 0;
            for (const QJsonValue& v : data) {
                if (v.isObject()) {
                    QJsonObject obj = v.toObject();
                    // Filter: Skip artists and albums based on __TYPE__ field
                    QString type = obj.value("__TYPE__").toString();
                    // Tracks have __TYPE__="song", albums have __TYPE__="album", artists have __TYPE__="artist"
                    bool isArtist = type == "artist";
                    bool isAlbum = type == "album";
                    bool isTrack = type == "song" || obj.contains("SNG_ID");

                    if (isArtist || isAlbum) {
                        skippedCount++;
                        continue;  // Skip non-track items
                    }

                    // Try to parse as track
                    tracks.append(parseTrack(obj));
                }
            }
            if (skippedCount > 0) {
                emit debugLog(QString("[search_music] Filtered out %1 non-track items").arg(skippedCount));
            }
            // Only emit generic tracksFound for non-context searches (legacy compatibility)
            if (!context) {
                emit tracksFound(tracks);
            }
            // Always emit context-aware signal
            emit searchTracksFound(tracks, context);
        }
        return;
    }

    if (method.startsWith("get_url:")) {
        QString trackId = method.mid(8);
        int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QJsonObject root = QJsonDocument::fromJson(data).object();
        // Diezel: if res.status !== 200, throw MediaClientError with res.data.errors
        if (status != 200) {
            QString msg = QString("Media API returned status %1").arg(status);
            if (root.contains("errors") && root["errors"].isArray()) {
                for (const QJsonValue& e : root["errors"].toArray()) {
                    QJsonObject err = e.toObject();
                    msg += QString("\n  %1: %2").arg(err["code"].toInt()).arg(err["message"].toString());
                }
            }
            emit debugLog(QString("[get_url] %1. Body: %2").arg(msg, rawResponse.left(500)));
            emit error(msg);
            return;
        }
        // Diezel: return res.data.data — array of { media: [ { sources: [{ url }], media_type, format, nbf, exp } ... ] }
        // When multiple formats requested, pick the best available (by STREAM_FORMAT_PREFERENCE).
        QJsonArray dataArr = root["data"].toArray();
        if (dataArr.isEmpty()) {
            emit error("Media API returned no data");
            return;
        }
        QJsonObject first = dataArr[0].toObject();
        QJsonArray mediaArr = first["media"].toArray();
        if (mediaArr.isEmpty()) {
            emit error("Media API: no media in response");
            return;
        }
        QStringList returnedFormats;
        for (const QJsonValue& m : mediaArr) {
            QJsonObject o = m.toObject();
            QString fmt = o["format"].toString();
            if (!fmt.isEmpty()) returnedFormats.append(fmt);
        }
        emit debugLog(QString("[get_url] Response media count: %1, formats: %2").arg(mediaArr.size()).arg(returnedFormats.join(',')));

        QJsonObject bestMedia;
        QString bestFormat;
        int bestOrder = STREAM_FORMAT_PREFERENCE.size();
        for (const QJsonValue& m : mediaArr) {
            QJsonObject media = m.toObject();
            QJsonArray sources = media["sources"].toArray();
            if (sources.isEmpty()) continue;
            QString fmt = media["format"].toString();
            int order = STREAM_FORMAT_PREFERENCE.indexOf(fmt);
            if (order < 0) order = STREAM_FORMAT_PREFERENCE.size();
            if (order < bestOrder) {
                bestOrder = order;
                bestMedia = media;
                bestFormat = fmt;
            }
        }
        if (bestMedia.isEmpty()) {
            emit error("Media API: no sources (stream URL) in response");
            return;
        }
        QString url = bestMedia["sources"].toArray()[0].toObject()["url"].toString();
        if (bestFormat.isEmpty())
            bestFormat = QStringLiteral("MP3_128");
        emit debugLog(QString("[get_url] Picked format: %1").arg(bestFormat));
        emit streamUrlReceived(trackId, url, bestFormat);
        return;
    }
}

QString DeezerAPI::imageUrlForObject(const QString& type, const QString& pictureId, int width, int height)
{
    if (pictureId.isEmpty()) return QString();
    QString typeStr = type;
    if (typeStr.isEmpty()) typeStr = "cover";
    return QString("%1/%2/%3/%4x%5-000000-80-0-0.jpg")
        .arg(IMAGE_BASE_URL).arg(typeStr).arg(pictureId).arg(width).arg(height);
}

std::shared_ptr<Track> DeezerAPI::parseTrack(const QJsonObject& trackJson)
{
    auto track = std::make_shared<Track>();

    QString trackId = trackJson.contains("SNG_ID") ?
        QString::number(trackJson["SNG_ID"].toVariant().toLongLong()) :
        QString::number(trackJson["id"].toInt());
    QString title = trackJson.contains("SNG_TITLE") ? trackJson["SNG_TITLE"].toString() : trackJson["title"].toString();
    track->setId(trackId);
    track->setTitle(title);
    int duration = trackJson.contains("DURATION") ? trackJson["DURATION"].toVariant().toInt() : trackJson["duration"].toVariant().toInt();
    track->setDuration(duration);
    if (trackJson.contains("ART_NAME"))
        track->setArtist(trackJson["ART_NAME"].toString());
    else if (trackJson.contains("artist"))
        track->setArtist(trackJson["artist"].toObject()["name"].toString());
    QString albumTitle = trackJson.contains("ALB_TITLE") ? trackJson["ALB_TITLE"].toString() : QString();
    if (trackJson.contains("album") && !trackJson["album"].isString())
        albumTitle = trackJson["album"].toObject()["title"].toString();
    track->setAlbum(albumTitle);

    // Extract album picture ID - try multiple sources
    QString picId = trackJson.contains("ALB_PICTURE") ? trackJson["ALB_PICTURE"].toString() : QString();

    // Try album object if ALB_PICTURE is missing
    if (picId.isEmpty() && trackJson.contains("album") && trackJson["album"].isObject()) {
        QJsonObject albumObj = trackJson["album"].toObject();

        // Try direct picture ID field (e.g., "picture")
        if (albumObj.contains("picture")) {
            QString picField = albumObj["picture"].toString();
            // Extract ID from URL like "https://api.deezer.com/album/123456/image" or just use the ID directly
            if (picField.contains('/')) {
                picId = picField.section('/', -2, -2);  // Second to last segment
                if (picId == "album") {
                    picId = picField.section('/', -1);  // Last segment if second-to-last was "album"
                }
            } else {
                picId = picField;
            }
        }

        // Try cover field (public API)
        if (picId.isEmpty() && albumObj.contains("cover")) {
            QString coverField = albumObj["cover"].toString();
            if (coverField.contains('/')) {
                // Extract ID from URL segments
                QStringList parts = coverField.split('/');
                // Look for numeric ID in URL
                for (int i = parts.size() - 1; i >= 0; --i) {
                    if (!parts[i].isEmpty() && !parts[i].contains('.')) {
                        bool isNumber;
                        parts[i].toLongLong(&isNumber);
                        if (isNumber || parts[i].length() >= 8) {
                            picId = parts[i];
                            break;
                        }
                    }
                }
            } else {
                picId = coverField;
            }
        }

        // Try other cover size fields as fallback
        if (picId.isEmpty()) {
            for (const QString& key : {"cover_medium", "cover_small", "cover_big", "cover_xl"}) {
                if (albumObj.contains(key)) {
                    QString url = albumObj[key].toString();
                    // Extract picture ID from URL like https://e-cdns-images.dzcdn.net/images/cover/abc123def/250x250-000000-80-0-0.jpg
                    QStringList parts = url.split('/');
                    for (const QString& part : parts) {
                        if (part.contains("images") || part.contains("cover")) continue;
                        if (part.length() >= 8 && !part.contains('.') && !part.contains('x')) {
                            picId = part;
                            break;
                        }
                    }
                    if (!picId.isEmpty()) break;
                }
            }
        }
    }

    QString albumArtUrl = imageUrlForObject("cover", picId, 1000, 1000);
    track->setAlbumArt(albumArtUrl);

    if (trackJson.contains("TRACK_TOKEN"))
        track->setTrackToken(trackJson["TRACK_TOKEN"].toString());
    track->setPreviewUrl(QString("https://cdns-preview-e.dzcdn.net/stream/c-%1-1.mp3").arg(trackId));

    // Favorite status - check against fetched favorites set
    if (!m_favoriteTrackIds.isEmpty())
        track->setFavorite(m_favoriteTrackIds.contains(trackId));

    return track;
}

std::shared_ptr<Playlist> DeezerAPI::parsePlaylist(const QJsonObject& playlistJson)
{
    auto playlist = std::make_shared<Playlist>();
    QString id = playlistJson.contains("PLAYLIST_ID") ?
        playlistJson["PLAYLIST_ID"].toVariant().toString() :
        playlistJson.value("id").toVariant().toString();

    QString title = playlistJson.contains("TITLE") ? playlistJson["TITLE"].toString() : playlistJson["title"].toString();
    QString desc = playlistJson.contains("DESCRIPTION") ? playlistJson["DESCRIPTION"].toString() : playlistJson.value("description").toString();
    playlist->setId(id);
    playlist->setTitle(title);
    playlist->setDescription(desc);
    // NB_SONG from playlist.getList; nb_tracks from public API
    int nbSongs = playlistJson.contains("NB_SONG") ? playlistJson["NB_SONG"].toInt()
                : playlistJson.value("nb_tracks").toInt(0);
    playlist->setTrackCount(nbSongs);
    // DATE_MOD from gateway is a date string like "2024-01-15 10:30:00"
    // time_mod from public API is a Unix timestamp integer
    if (playlistJson.contains("DATE_MOD")) {
        QString dateStr = playlistJson["DATE_MOD"].toString();
        QDateTime dt = QDateTime::fromString(dateStr, "yyyy-MM-dd HH:mm:ss");
        if (!dt.isValid())
            dt = QDateTime::fromString(dateStr, Qt::ISODate);
        if (dt.isValid())
            playlist->setLastModified(dt);
    } else if (playlistJson.contains("DATE_LAST_MODIFY")) {
        QString dateStr = playlistJson["DATE_LAST_MODIFY"].toString();
        QDateTime dt = QDateTime::fromString(dateStr, "yyyy-MM-dd HH:mm:ss");
        if (dt.isValid())
            playlist->setLastModified(dt);
    } else if (playlistJson.contains("DATE_LAST_UPDATE")) {
        QString dateStr = playlistJson["DATE_LAST_UPDATE"].toString();
        QDateTime dt = QDateTime::fromString(dateStr, "yyyy-MM-dd HH:mm:ss");
        if (dt.isValid())
            playlist->setLastModified(dt);
    } else if (playlistJson.contains("time_mod")) {
        qint64 ts = playlistJson["time_mod"].toVariant().toLongLong();
        if (ts > 0) playlist->setLastModified(QDateTime::fromSecsSinceEpoch(ts));
    }
    
    if (playlistJson.contains("DURATION")) {
        playlist->setTotalDuration(playlistJson["DURATION"].toVariant().toInt());
    } else if (playlistJson.contains("PLAYLIST_DURATION")) {
        playlist->setTotalDuration(playlistJson["PLAYLIST_DURATION"].toVariant().toInt());
    } else if (playlistJson.contains("duration")) {
        playlist->setTotalDuration(playlistJson["duration"].toVariant().toInt());
    }
    
    QString picId = playlistJson.contains("PLAYLIST_PICTURE") ? playlistJson["PLAYLIST_PICTURE"].toString() : QString();
    if (picId.isEmpty() && playlistJson.contains("checksum"))
        picId = playlistJson["checksum"].toString();
    if (picId.isEmpty() && playlistJson.contains("picture_small"))
        picId = playlistJson["picture_small"].toString().section('/', -2, -2);
        
    if (!picId.isEmpty())
        playlist->setCoverUrl(imageUrlForObject("playlist", picId, 1000, 1000));

    return playlist;
}

std::shared_ptr<Album> DeezerAPI::parseAlbum(const QJsonObject& albumJson)
{
    auto album = std::make_shared<Album>();

    QString id = albumJson.contains("ALB_ID") ?
        QString::number(albumJson["ALB_ID"].toVariant().toLongLong()) :
        QString::number(albumJson["id"].toVariant().toLongLong());
    QString title = albumJson.contains("ALB_TITLE") ? albumJson["ALB_TITLE"].toString() : albumJson["title"].toString();
    album->setId(id);
    album->setTitle(title);
    
    QString artist;
    if (albumJson.contains("ART_NAME"))
        artist = albumJson["ART_NAME"].toString();
    else if (albumJson.contains("artist"))
        artist = albumJson["artist"].isObject() ? albumJson["artist"].toObject()["name"].toString() : albumJson["artist"].toString();
    album->setArtist(artist);

    if (albumJson.contains("ALB_RELEASE_DATE")) {
        album->setReleaseDate(albumJson["ALB_RELEASE_DATE"].toString());
    } else if (albumJson.contains("PHYSICAL_RELEASE_DATE")) {
        album->setReleaseDate(albumJson["PHYSICAL_RELEASE_DATE"].toString());
    } else if (albumJson.contains("release_date")) {
        album->setReleaseDate(albumJson["release_date"].toString());
    }
    
    QString picId = albumJson.contains("ALB_PICTURE") ? albumJson["ALB_PICTURE"].toString() : QString();
    if (picId.isEmpty() && albumJson.contains("cover"))
        picId = albumJson["cover"].isString() ? albumJson["cover"].toString().section('/', -2, -2) : albumJson["cover_small"].toString().section('/', -2, -2);

    QString coverUrl;
    if (!picId.isEmpty()) {
        coverUrl = imageUrlForObject("cover", picId, 1000, 1000);
        album->setCoverUrl(coverUrl);
    }

    return album;
}

QString DeezerAPI::md5Hex(const QString& input)
{
    QByteArray hash = QCryptographicHash::hash(input.toUtf8(), QCryptographicHash::Md5);
    return QString::fromLatin1(hash.toHex());
}

// BF_CBC_STRIPE — match jukebox-player-vlc decrypter.js + blowfish-cbc.js exactly.
// - Key: MD5(Buffer.from(String(trackId), 'binary')) as hex, then bfKey[i] = idMd5[i]^idMd5[i+16]^SECRET[i].
// - Chunks of 2048 bytes; decrypt when (block % 3 == 0) per decrypter.js line 49.
// - Blowfish CBC from jukebox (same key expansion, same IV [0,1,2,3,4,5,6,7] per chunk).
bool DeezerAPI::decryptStreamBuffer(QByteArray& data, const QString& trackId) const
{
    QByteArray keyBytes = s_trackXorKey.toLatin1();
    if (keyBytes.size() < 16 || data.isEmpty()) return false;
    // Buffer.from(String(trackId), 'binary') => trackId as Latin1 bytes
    QByteArray idBytes = trackId.toLatin1();
    QString songIdHash = QString::fromLatin1(QCryptographicHash::hash(idBytes, QCryptographicHash::Md5).toHex());
    if (songIdHash.size() < 32) return false;
    QByteArray trackKey(16, 0);
    for (int i = 0; i < 16; i++) {
        quint8 c = static_cast<quint8>(songIdHash[i].unicode());
        c ^= static_cast<quint8>(songIdHash[i + 16].unicode());
        c ^= static_cast<quint8>(keyBytes[i]);
        trackKey[i] = c;
    }
    static const quint8 IV[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    const int BLOCK_SIZE = 2048;
    int chunkIndex = 0;
    int offset = 0;
    while (offset + BLOCK_SIZE <= data.size()) {
        if (chunkIndex % 3 == 0) {
            blowfishCbcDecryptChunk(reinterpret_cast<const quint8*>(trackKey.constData()), IV, reinterpret_cast<quint8*>(data.data() + offset));
        }
        offset += BLOCK_SIZE;
        chunkIndex++;
    }
    return true;
}

