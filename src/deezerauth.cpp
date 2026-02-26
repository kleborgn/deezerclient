#include "deezerauth.h"
#include <QNetworkRequest>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QDebug>

#ifdef DEEZER_OPENSSL
#include <openssl/evp.h>
#include <openssl/aes.h>
#endif
#include "secrets.h"

const QString DeezerAuth::GATEWAY_URL = "https://api.deezer.com/1.0/gateway.php";
const QString DeezerAuth::WEB_GATEWAY_URL = "https://www.deezer.com/ajax/gw-light.php";

QString DeezerAuth::s_mobileGwKey = DEEZER_MOBILE_GW_KEY;

// Diezel-style User-Agent (Android)
static const char* USER_AGENT = "Deezer/6.1.22.49 (Android; 9; Tablet; us) innotek GmbH VirtualBox";

static QString generateNonce(int length)
{
    static const char alphabet[] = "012345689abdef";
    QString out;
    out.reserve(length);
    auto* gen = QRandomGenerator::global();
    for (int i = 0; i < length; i++)
        out += QChar(alphabet[gen->bounded(static_cast<int>(sizeof(alphabet) - 1))]);
    return out;
}

// USER_ID can exceed 32-bit int (e.g. 6399668623); parse as string or 64-bit
static QString userIdFromJson(const QJsonValue& v)
{
    if (v.isString())
        return v.toString().trimmed();
    if (v.isDouble())
        return QString::number(static_cast<qint64>(v.toDouble()));
    return QString::number(v.toVariant().toLongLong());
}

static QString md5Hex(const QString& input)
{
    QByteArray hash = QCryptographicHash::hash(input.toUtf8(), QCryptographicHash::Md5);
    return QString::fromLatin1(hash.toHex());
}

DeezerAuth::DeezerAuth(QObject *parent)
    : QObject(parent)
    , m_networkManager(new QNetworkAccessManager(this))
    , m_authenticated(false)
    , m_pendingArlAutoLog(false)
    , m_pendingEmailLogin(false)
    , m_pendingCheckFormRefresh(false)
{
    connect(m_networkManager, &QNetworkAccessManager::finished,
            this, &DeezerAuth::handleNetworkReply);
}

DeezerAuth::~DeezerAuth()
{
}

void DeezerAuth::setMobileGwKey(const QString& key)
{
    s_mobileGwKey = key;
}

void DeezerAuth::signInWithEmail(const QString& email, const QString& password)
{
    if (s_mobileGwKey.isEmpty()) {
        emit authenticationFailed("MOBILE_GW_KEY required for email login.");
        return;
    }
    m_pendingEmailLogin = true;
    m_pendingEmail = email;
    m_pendingPassword = password;
    initializeKeys();  // async: mobile_auth -> api_checkToken -> then we call mobile_userAuth
}

void DeezerAuth::signInWithArl(const QString& arl)
{
    m_arl = arl;
    fetchWebUserData();
}

void DeezerAuth::logout()
{
    m_authenticated = false;
    m_sid.clear();
    m_webSid.clear();
    m_arl.clear();
    m_userId.clear();
    m_username.clear();
    m_licenseToken.clear();
    m_mediaUrl.clear();
    m_checkForm.clear();
    m_pendingArlAutoLog = false;
    m_pendingEmailLogin = false;
    m_pendingCheckFormRefresh = false;
    m_pendingEmail.clear();
    m_pendingPassword.clear();
    m_userKey.clear();
}

QString DeezerAuth::buildCookieString() const
{
    QStringList cookies;
    if (!m_arl.isEmpty()) cookies << QString("arl=%1").arg(m_arl);

    // Prefer Web SID if available (for gw-light.php calls), otherwise Mobile SID
    if (!m_webSid.isEmpty()) {
        cookies << QString("sid=%1").arg(m_webSid);
    } else if (!m_sid.isEmpty()) {
        cookies << QString("sid=%1").arg(m_sid);
    }
    return cookies.join("; ");
}

void DeezerAuth::initializeKeys()
{
    QString uniqId = generateNonce(32);
    QUrlQuery q;
    q.addQueryItem("uniq_id", uniqId);
    callGatewayGet("mobile_auth", q);
}

void DeezerAuth::fetchWebUserData()
{
    // Use web gateway to get USER_ID from ARL and checkForm (CSRF token)
    QUrl url(WEB_GATEWAY_URL);
    QUrlQuery query;
    query.addQueryItem("api_version", "1.0");
    query.addQueryItem("api_token", ""); // Empty string to request new token (as per web client)
    query.addQueryItem("input", "3");
    query.addQueryItem("output", "3");
    query.addQueryItem("cid", QString::number(QRandomGenerator::global()->generate()));
    query.addQueryItem("method", "deezer.getUserData");
    url.setQuery(query);

    QJsonObject params; // empty for getUserData
    QByteArray postData = QJsonDocument(params).toJson(QJsonDocument::Compact);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("User-Agent", USER_AGENT);
    request.setRawHeader("X-Requested-With", "XMLHttpRequest");

    QString cookies = buildCookieString();
    if (!cookies.isEmpty())
        request.setRawHeader("Cookie", cookies.toUtf8());

    QNetworkReply* reply = m_networkManager->post(request, postData);
    m_pendingRequests[reply] = "deezer.getUserData";

    emit debugLog("[deezer.getUserData] Request sent");
}

void DeezerAuth::callGatewayGet(const QString& method, const QUrlQuery& extraParams)
{
    // Note: This needs the mobile API key which should be passed from DeezerAPI
    // For now, we'll use a static member or require it to be set
    QUrl url(GATEWAY_URL);
    QUrlQuery query;
    query.addQueryItem("api_key", DEEZER_MOBILE_API_KEY);
    query.addQueryItem("output", "3");
    query.addQueryItem("method", method);
    for (const auto& item : extraParams.queryItems(QUrl::FullyDecoded))
        query.addQueryItem(item.first, item.second);
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", USER_AGENT);

    QNetworkReply* reply = m_networkManager->get(request);
    m_pendingRequests[reply] = method;
    m_pendingGetParams[reply] = extraParams;
    emit debugLog(QString("[%1] Request sent (GET)").arg(method));
}

void DeezerAuth::callWebGatewayMethod(const QString& method, const QJsonObject& params)
{
    QUrl url(WEB_GATEWAY_URL);
    QUrlQuery query;
    query.addQueryItem("api_version", "1.0");
    query.addQueryItem("api_token", m_checkForm.isEmpty() ? "null" : m_checkForm);
    query.addQueryItem("input", "3");
    query.addQueryItem("output", "3");
    query.addQueryItem("cid", QString::number(QRandomGenerator::global()->generate()));
    query.addQueryItem("method", method);
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("User-Agent", USER_AGENT);
    request.setRawHeader("X-Requested-With", "XMLHttpRequest");

    QString cookies = buildCookieString();
    if (!cookies.isEmpty())
        request.setRawHeader("Cookie", cookies.toUtf8());

    QByteArray postData = QJsonDocument(params).toJson(QJsonDocument::Compact);
    QNetworkReply* reply = m_networkManager->post(request, postData);
    m_pendingRequests[reply] = method;

    emit debugLog(QString("[%1] Request sent (Web Gateway POST)").arg(method));
}

QString DeezerAuth::decryptLicense(const QString& encryptedHex, const QString& description, const QString& serial)
{
    if (encryptedHex.isEmpty()) return QString();
#ifdef DEEZER_OPENSSL
    QString hDescHex = md5Hex(description);
    QString hSerialHex = md5Hex(serial);
    if (hDescHex.size() < 32 || hSerialHex.size() < 32) return QString();
    QByteArray xorKey = hDescHex.left(16).toUtf8();
    if (xorKey.size() < 16) xorKey.resize(16);
    QByteArray d0 = hDescHex.mid(16, 16).toUtf8();
    QByteArray d1 = hSerialHex.left(16).toUtf8();
    QByteArray d2 = hSerialHex.mid(16, 16).toUtf8();
    if (d0.size() < 16) d0.resize(16);
    if (d1.size() < 16) d1.resize(16);
    if (d2.size() < 16) d2.resize(16);
    QByteArray decryptionKey(16, 0);
    for (int i = 0; i < 16; i++) {
        quint8 c = static_cast<quint8>(xorKey[i]);
        if (i < d0.size()) c ^= static_cast<quint8>(d0[i]);
        if (i < d1.size()) c ^= static_cast<quint8>(d1[i]);
        if (i < d2.size()) c ^= static_cast<quint8>(d2[i]);
        decryptionKey[i] = c;
    }
    QByteArray encryptedBytes = QByteArray::fromHex(encryptedHex.toLatin1());
    if (encryptedBytes.isEmpty() || (encryptedBytes.size() % 16) != 0) return QString();
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    int outLen = 0;
    QByteArray decrypted(encryptedBytes.size() + 16, 0);
    EVP_DecryptInit_ex(ctx, EVP_aes_128_ecb(), nullptr,
                       reinterpret_cast<const unsigned char*>(decryptionKey.constData()), nullptr);
    EVP_CIPHER_CTX_set_padding(ctx, 0);
    EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char*>(decrypted.data()), &outLen,
                      reinterpret_cast<const unsigned char*>(encryptedBytes.constData()), encryptedBytes.size());
    int finalLen = 0;
    EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(decrypted.data()) + outLen, &finalLen);
    EVP_CIPHER_CTX_free(ctx);
    decrypted.resize(outLen + finalLen);
    // Strip padding like diezel: find last non-zero byte, keep up to and including it
    int lastNonZero = -1;
    for (int i = decrypted.size() - 1; i >= 0; i--) {
        if (decrypted[i] != 0) { lastNonZero = i; break; }
    }
    if (lastNonZero >= 0)
        decrypted.resize(lastNonZero + 1);
    QJsonDocument doc = QJsonDocument::fromJson(decrypted);
    if (!doc.isObject()) return QString();
    QJsonObject root = doc.object();
    QJsonObject licence = root["LICENCE"].toObject();
    if (licence.isEmpty()) licence = root;
    QJsonObject options = licence["OPTIONS"].toObject();
    return options["license_token"].toString();
#else
    Q_UNUSED(description);
    Q_UNUSED(serial);
    return QString();
#endif
}

void DeezerAuth::handleNetworkReply(QNetworkReply* reply)
{
    reply->deleteLater();
    QString method = m_pendingRequests.value(reply, "");
    m_pendingRequests.remove(reply);
    m_pendingGetParams.remove(reply);

    if (reply->error() != QNetworkReply::NoError) {
        emit authenticationFailed(reply->errorString());
        return;
    }

    QByteArray data = reply->readAll();
    QString rawResponse = QString::fromUtf8(data);

    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject()) {
        emit debugLog(QString("[%1] Invalid JSON. Raw: %2").arg(method, rawResponse.left(500)));
        emit authenticationFailed("Invalid JSON response");
        return;
    }
    QJsonObject obj = doc.object();

    // Check for API errors
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
                    else msg += QString("%1: (complex)\n").arg(it.key());
                }
            }
        } else if (errVal.isString()) {
            msg = errVal.toString().trimmed();
            if (!msg.isEmpty()) isError = true;
        }
        if (isError) {
            if (msg.isEmpty()) msg = "API error";
            emit debugLog(QString("[%1] API error: %2").arg(method, msg));
            emit authenticationFailed(msg);
            return;
        }
    }

    QJsonValue resultsVal = obj["results"];
    if (resultsVal.isUndefined())
        resultsVal = obj["result"];
    if (resultsVal.isUndefined() && obj.contains("data"))
        resultsVal = obj["data"];
    if (resultsVal.isUndefined() && method == "mobile_auth")
        resultsVal = QJsonValue(obj);

    if (method == "mobile_auth") {
        handleMobileAuth(resultsVal.toObject());
    } else if (method == "api_checkToken") {
        handleApiCheckToken(resultsVal);
    } else if (method == "mobile_userAuth") {
        handleMobileUserAuth(resultsVal.toObject());
    } else if (method == "mobile_userAutoLog") {
        handleMobileUserAutoLog(resultsVal.toObject());
    } else if (method == "deezer.getUserData") {
        handleGetUserData(reply, obj, resultsVal.toObject());
    }
}

void DeezerAuth::handleMobileAuth(const QJsonObject& results)
{
    QJsonObject config = results["CONFIG"].toObject();
    m_mediaUrl = config["URL_MEDIA"].toString();
    if (m_mediaUrl.isEmpty())
        m_mediaUrl = results["URL_MEDIA"].toString();
    if (m_mediaUrl.isEmpty())
        emit debugLog(QString("[mobile_auth] URL_MEDIA empty. CONFIG keys: %1").arg(QStringList(config.keys()).join(',')));

    QString tokenHex = results["TOKEN"].toString();
    if (tokenHex.isEmpty())
        tokenHex = config["TOKEN"].toString();

#ifndef DEEZER_OPENSSL
    emit authenticationFailed("OpenSSL required for mobile login. Build with OpenSSL or use ARL login (web gateway only).");
    return;
#endif

#ifdef DEEZER_OPENSSL
    if (s_mobileGwKey.isEmpty()) {
        emit authenticationFailed("MOBILE_GW_KEY required to decrypt token");
        return;
    }
    if (tokenHex.isEmpty()) {
        emit authenticationFailed("No TOKEN in mobile_auth response. Check MOBILE_API_KEY.");
        return;
    }

    // Decrypt the token using AES-128-ECB with MOBILE_GW_KEY
    QString keyStr = s_mobileGwKey;
    keyStr.remove(QChar(' '));
    QByteArray keyBytes = QByteArray::fromHex(keyStr.toLatin1());
    if (keyBytes.size() < 16) {
        keyBytes = keyStr.toUtf8();
        if (keyBytes.size() < 16) {
            emit authenticationFailed("MOBILE_GW_KEY must be 16 bytes (16 characters) or 32 hex characters.");
            return;
        }
    }
    if (keyBytes.size() > 16)
        keyBytes = keyBytes.left(16);

    QByteArray tokenBytes = QByteArray::fromHex(tokenHex.toLatin1());
    if (tokenBytes.isEmpty()) {
        emit authenticationFailed("TOKEN from server is not valid hex. Check MOBILE_API_KEY.");
        return;
    }

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    const int blockSize = 16;
    int outLen = 0;
    QByteArray decrypted((tokenBytes.size() / blockSize + 1) * blockSize, 0);
    EVP_DecryptInit_ex(ctx, EVP_aes_128_ecb(), nullptr, reinterpret_cast<const unsigned char*>(keyBytes.constData()), nullptr);
    EVP_CIPHER_CTX_set_padding(ctx, 0);
    EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char*>(decrypted.data()), &outLen,
                     reinterpret_cast<const unsigned char*>(tokenBytes.constData()), tokenBytes.size());
    int finalLen = 0;
    EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(decrypted.data()) + outLen, &finalLen);
    EVP_CIPHER_CTX_free(ctx);
    decrypted.resize(outLen + finalLen);

    if (decrypted.size() < 96) {
        emit authenticationFailed("Decrypted token too short.");
        return;
    }

    m_userKey = decrypted.mid(80, 16);  // userKey for encrypting password
    QString decStr = QString::fromUtf8(decrypted);
    QString tokenPart = decStr.mid(0, 64);
    QString tokenKeyPart = decStr.mid(64, 16);
    QByteArray tokenKeyBytes = tokenKeyPart.toUtf8();
    if (tokenKeyBytes.size() < 16) tokenKeyBytes.resize(16);

    QByteArray tokenPartBytes = tokenPart.toUtf8();
    int pad = 16 - (tokenPartBytes.size() % 16);
    if (pad < 16) tokenPartBytes.append(pad, '\0');

    QByteArray encryptedToken(tokenPartBytes.size() + 16, 0);
    int encLen = 0;
    EVP_CIPHER_CTX* ctx2 = EVP_CIPHER_CTX_new();
    EVP_EncryptInit_ex(ctx2, EVP_aes_128_ecb(), nullptr, reinterpret_cast<const unsigned char*>(tokenKeyBytes.constData()), nullptr);
    EVP_CIPHER_CTX_set_padding(ctx2, 0);
    EVP_EncryptUpdate(ctx2, reinterpret_cast<unsigned char*>(encryptedToken.data()), &encLen,
                      reinterpret_cast<const unsigned char*>(tokenPartBytes.constData()), tokenPartBytes.size());
    int encFinal = 0;
    EVP_EncryptFinal_ex(ctx2, reinterpret_cast<unsigned char*>(encryptedToken.data()) + encLen, &encFinal);
    EVP_CIPHER_CTX_free(ctx2);
    encryptedToken.resize(encLen + encFinal);

    QString authTokenHex = QString::fromLatin1(encryptedToken.toHex());
    QUrlQuery q;
    q.addQueryItem("auth_token", authTokenHex);
    callGatewayGet("api_checkToken", q);
#endif
}

void DeezerAuth::handleApiCheckToken(const QJsonValue& resultsVal)
{
    if (resultsVal.isString()) {
        m_sid = resultsVal.toString();
    } else {
        m_sid = resultsVal.toObject().value("sid").toString();
    }

    if (m_pendingArlAutoLog && !m_arl.isEmpty() && !m_userId.isEmpty()) {
        m_pendingArlAutoLog = false;
        QJsonObject params;
        params["ARL"] = m_arl;
        params["ACCOUNT_ID"] = m_userId;
        params["device_serial"] = "";
        params["platform"] = "innotek GmbH_x86_64_9";
        params["custo_version_id"] = "";
        params["custo_partner"] = "";
        params["model"] = "VirtualBox";
        params["device_name"] = "VirtualBox";
        params["device_os"] = "Android";
        params["device_type"] = "tablet";
        params["google_play_services_availability"] = "1";
        params["consent_string"] = "";

        // Call mobile_userAutoLog through gateway
        QUrl url(GATEWAY_URL);
        QUrlQuery query;
        query.addQueryItem("api_key", DEEZER_MOBILE_API_KEY);
        query.addQueryItem("output", "3");
        query.addQueryItem("input", "3");
        query.addQueryItem("method", "mobile_userAutoLog");
        query.addQueryItem("sid", m_sid);
        url.setQuery(query);

        QByteArray postData = QJsonDocument(params).toJson(QJsonDocument::Compact);
        QNetworkRequest request(url);
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        request.setRawHeader("User-Agent", USER_AGENT);

        QNetworkReply* reply = m_networkManager->post(request, postData);
        m_pendingRequests[reply] = "mobile_userAutoLog";
        emit debugLog("[mobile_userAutoLog] Request sent (POST)");
    }
    else if (m_pendingEmailLogin && !m_pendingEmail.isEmpty() && m_userKey.size() == 16) {
#ifdef DEEZER_OPENSSL
        m_pendingEmailLogin = false;
        QString email = m_pendingEmail;
        QByteArray passBytes = m_pendingPassword.toUtf8();
        m_pendingEmail.clear();
        m_pendingPassword.clear();

        int padLen = 16 - (passBytes.size() % 16);
        if (padLen < 16) passBytes.append(padLen, '\0');

        QByteArray encPass(passBytes.size() + 16, 0);
        int encLen = 0;
        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        EVP_EncryptInit_ex(ctx, EVP_aes_128_ecb(), nullptr, reinterpret_cast<const unsigned char*>(m_userKey.constData()), nullptr);
        EVP_CIPHER_CTX_set_padding(ctx, 0);
        EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char*>(encPass.data()), &encLen, reinterpret_cast<const unsigned char*>(passBytes.constData()), passBytes.size());
        int encFinal = 0;
        EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(encPass.data()) + encLen, &encFinal);
        EVP_CIPHER_CTX_free(ctx);
        encPass.resize(encLen + encFinal);

        QString passwordHex = QString::fromLatin1(encPass.toHex());
        QJsonObject params;
        params["mail"] = email;
        params["password"] = passwordHex;
        params["device_serial"] = "";
        params["platform"] = "innotek GmbH_x86_64_9";
        params["custo_version_id"] = "";
        params["custo_partner"] = "";
        params["model"] = "VirtualBox";
        params["device_name"] = "VirtualBox";
        params["device_os"] = "Android";
        params["device_type"] = "tablet";
        params["google_play_services_availability"] = "1";
        params["consent_string"] = "";

        QUrl url(GATEWAY_URL);
        QUrlQuery query;
        query.addQueryItem("api_key", DEEZER_MOBILE_API_KEY);
        query.addQueryItem("output", "3");
        query.addQueryItem("input", "3");
        query.addQueryItem("method", "mobile_userAuth");
        query.addQueryItem("sid", m_sid);
        url.setQuery(query);

        QByteArray postData = QJsonDocument(params).toJson(QJsonDocument::Compact);
        QNetworkRequest request(url);
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        request.setRawHeader("User-Agent", USER_AGENT);

        QNetworkReply* reply = m_networkManager->post(request, postData);
        m_pendingRequests[reply] = "mobile_userAuth";
        emit debugLog("[mobile_userAuth] Request sent (POST)");
#endif
    }
}

void DeezerAuth::handleMobileUserAuth(const QJsonObject& results)
{
    m_userId = userIdFromJson(results["USER_ID"]);
    m_username = results["BLOG_NAME"].toString();
    m_arl = results["ARL"].toString();
    m_authenticated = true;
    emit debugLog(QString("Login (mobile_userAuth): user_id=%1, username=%2").arg(m_userId, m_username));

    if (results.contains("PREMIUM")) {
        QJsonObject premium = results["PREMIUM"].toObject();
        if (premium.contains("OPTIONS"))
            m_licenseToken = premium["OPTIONS"].toObject()["license_token"].toString();
        if (m_licenseToken.isEmpty() && premium.contains("RANDOM")) {
            QString desc = results["DESCRIPTION"].toString();
            QString token = decryptLicense(premium["RANDOM"].toString(), desc, QString());
            if (!token.isEmpty()) m_licenseToken = token;
        }
    }
    if (m_licenseToken.isEmpty())
        emit debugLog(QString("[mobile_userAuth] license_token empty (need for full streams). PREMIUM keys: %1").arg(results.contains("PREMIUM") ? QStringList(results["PREMIUM"].toObject().keys()).join(',') : QString()));

    // Fetch web user data to get checkForm (CSRF) for web gateway calls
    m_pendingCheckFormRefresh = true;
    fetchWebUserData();
}

void DeezerAuth::handleMobileUserAutoLog(const QJsonObject& results)
{
    m_userId = userIdFromJson(results["USER_ID"]);
    m_username = results["BLOG_NAME"].toString();
    m_arl = results["ARL"].toString();
    m_authenticated = true;
    emit debugLog(QString("Login (mobile_userAutoLog): user_id=%1, username=%2").arg(m_userId, m_username));

    if (results.contains("PREMIUM")) {
        QJsonObject premium = results["PREMIUM"].toObject();
        if (premium.contains("OPTIONS"))
            m_licenseToken = premium["OPTIONS"].toObject()["license_token"].toString();
        if (m_licenseToken.isEmpty() && premium.contains("RANDOM")) {
            QString desc = results["DESCRIPTION"].toString();
            QString token = decryptLicense(premium["RANDOM"].toString(), desc, QString());
            if (!token.isEmpty()) m_licenseToken = token;
        }
    }
    if (m_licenseToken.isEmpty())
        emit debugLog(QString("[mobile_userAutoLog] license_token empty (need for full streams). PREMIUM keys: %1").arg(results.contains("PREMIUM") ? QStringList(results["PREMIUM"].toObject().keys()).join(',') : QString()));

    // Refresh web user data to get fresh checkForm (CSRF token) for web gateway calls
    m_pendingCheckFormRefresh = true;
    fetchWebUserData();
}

void DeezerAuth::handleGetUserData(QNetworkReply* reply, const QJsonObject& obj, const QJsonObject& results)
{
    // Capture Set-Cookie headers to get Web SID
    QString cookieStr = QString::fromUtf8(reply->rawHeader("Set-Cookie"));
    int sidIdx = cookieStr.indexOf("sid=");
    if (sidIdx != -1) {
        int endIdx = cookieStr.indexOf(';', sidIdx);
        if (endIdx == -1) endIdx = cookieStr.length();
        m_webSid = cookieStr.mid(sidIdx + 4, endIdx - (sidIdx + 4));
        emit debugLog(QString("Web SID captured: %1").arg(m_webSid));
    }

    if (results.contains("USER")) {
        QJsonObject user = results["USER"].toObject();
        m_userId = userIdFromJson(user["USER_ID"]);
        m_username = user["BLOG_NAME"].toString();
        m_authenticated = true;

        if (results.contains("checkForm")) {
            m_checkForm = results["checkForm"].toString();
        } else if (results.contains("checkFormLogin")) {
            m_checkForm = results["checkFormLogin"].toString();
        } else if (obj.contains("checkForm")) {
            m_checkForm = obj["checkForm"].toString();
        }

        emit debugLog(QString("Login (deezer.getUserData/ARL): user_id=%1, username=%2, checkForm=%3")
            .arg(m_userId, m_username, m_checkForm.isEmpty() ? "MISSING" : "FOUND"));

        if (user.contains("OPTIONS")) {
            m_licenseToken = user["OPTIONS"].toObject()["license_token"].toString();
        }

        // Check if this is a checkForm refresh after mobile auth completed
        if (m_pendingCheckFormRefresh) {
            m_pendingCheckFormRefresh = false;
            emit debugLog("[deezer.getUserData] checkForm refreshed after mobile auth, emitting authenticated");
            emit authenticated(m_username);
            return;
        }

#ifdef DEEZER_OPENSSL
        // Get mobile SID and full session (license, media URL) via mobile gateway.
        // Only trigger mobile auth flow if we don't already have a mobile SID
        if (m_sid.isEmpty()) {
            m_pendingArlAutoLog = true;
            initializeKeys();
        }
#else
        // Without OpenSSL there is no mobile flow; emit authenticated now.
        emit authenticated(m_username);
#endif
    } else {
        emit authenticationFailed("Invalid ARL or session");
    }
}
