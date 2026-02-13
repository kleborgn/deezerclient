#ifndef DEEZERAUTH_H
#define DEEZERAUTH_H

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QUrlQuery>
#include <QMap>
#include <QJsonObject>
#include <QJsonValue>

/**
 * Handles Deezer authentication (email/password and ARL login)
 * Supports both mobile gateway auth and web gateway auth
 */
class DeezerAuth : public QObject
{
    Q_OBJECT

public:
    explicit DeezerAuth(QObject *parent = nullptr);
    ~DeezerAuth();

    // Authentication methods
    void signInWithEmail(const QString& email, const QString& password);
    void signInWithArl(const QString& arl);
    void logout();

    // Session info
    bool isAuthenticated() const { return m_authenticated; }
    QString sid() const { return m_sid; }
    QString webSid() const { return m_webSid; }
    QString arl() const { return m_arl; }
    QString userId() const { return m_userId; }
    QString username() const { return m_username; }
    QString licenseToken() const { return m_licenseToken; }
    QString checkForm() const { return m_checkForm; }
    QString mediaUrl() const { return m_mediaUrl; }

    // Cookie helper
    QString buildCookieString() const;

    // Static configuration
    static void setMobileGwKey(const QString& key);
    static QString mobileGwKey() { return s_mobileGwKey; }

signals:
    void authenticated(const QString& username);
    void authenticationFailed(const QString& error);
    void debugLog(const QString& message);

private slots:
    void handleNetworkReply(QNetworkReply* reply);

private:
    void initializeKeys();
    void fetchWebUserData();
    void callGatewayGet(const QString& method, const QUrlQuery& extraParams);
    void callWebGatewayMethod(const QString& method, const QJsonObject& params);
    QString decryptLicense(const QString& encryptedHex, const QString& description, const QString& serial);

    // Network reply handlers
    void handleMobileAuth(const QJsonObject& results);
    void handleApiCheckToken(const QJsonValue& resultsVal);
    void handleMobileUserAuth(const QJsonObject& results);
    void handleMobileUserAutoLog(const QJsonObject& results);
    void handleGetUserData(QNetworkReply* reply, const QJsonObject& obj, const QJsonObject& results);

    QNetworkAccessManager* m_networkManager;
    QString m_sid;          // Mobile SID
    QString m_webSid;       // Web SID
    QString m_arl;
    QString m_userId;
    QString m_username;
    QString m_licenseToken;
    QString m_mediaUrl;     // From mobile_auth CONFIG.URL_MEDIA
    QString m_checkForm;    // CSRF token for web gateway
    QByteArray m_userKey;   // From mobile_auth decrypt (bytes 80-96)

    bool m_authenticated;
    bool m_pendingArlAutoLog;
    bool m_pendingEmailLogin;
    bool m_pendingCheckFormRefresh;
    QString m_pendingEmail;
    QString m_pendingPassword;

    QMap<QNetworkReply*, QString> m_pendingRequests;
    QMap<QNetworkReply*, QUrlQuery> m_pendingGetParams;

    static QString s_mobileGwKey;

    static const QString GATEWAY_URL;
    static const QString WEB_GATEWAY_URL;
};

#endif // DEEZERAUTH_H
