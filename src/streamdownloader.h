#ifndef STREAMDOWNLOADER_H
#define STREAMDOWNLOADER_H

#include <QObject>
#include <QByteArray>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

/**
 * Runs in a worker thread. Performs HTTPS GET and emits downloadReady(data, errorMsg)
 * so the main thread is never blocked by DNS/SSL/socket.
 */
class StreamDownloader : public QObject
{
    Q_OBJECT

public:
    explicit StreamDownloader(QObject* parent = nullptr);
    ~StreamDownloader();

public slots:
    void startDownload(const QString& url, const QString& trackId);

signals:
    void downloadReady(const QByteArray& data, const QString& errorMessage, const QString& trackId);

private slots:
    void onReplyFinished();

private:
    QNetworkAccessManager* m_nam;
    QNetworkReply* m_reply;
};

#endif // STREAMDOWNLOADER_H
