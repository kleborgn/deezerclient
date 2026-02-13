#include "streamdownloader.h"
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrl>

static const char* USER_AGENT = "Deezer/6.1.22.49 (Android; 9; Tablet; us) innotek GmbH VirtualBox";

StreamDownloader::StreamDownloader(QObject* parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
    , m_reply(nullptr)
{
}

StreamDownloader::~StreamDownloader()
{
    if (m_reply) {
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
}

void StreamDownloader::startDownload(const QString& url, const QString& trackId)
{
    if (m_reply) {
        QNetworkReply* oldReply = m_reply;
        m_reply = nullptr;
        oldReply->abort();
        oldReply->deleteLater();
    }
    QUrl qurl(url);
    QNetworkRequest req(qurl);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setRawHeader("User-Agent", USER_AGENT);
    m_reply = m_nam->get(req);
    m_reply->setProperty("trackId", trackId);
    connect(m_reply, &QNetworkReply::finished, this, &StreamDownloader::onReplyFinished);
}

void StreamDownloader::onReplyFinished()
{
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;
    m_reply = nullptr;
    reply->deleteLater();

    QString err;
    if (reply->error() != QNetworkReply::NoError)
        err = reply->errorString();
    QByteArray data = reply->readAll();
    QString trackId = reply->property("trackId").toString();
    emit downloadReady(data, err, trackId);
}
