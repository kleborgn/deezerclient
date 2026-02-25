#ifndef STREAMDOWNLOADER_H
#define STREAMDOWNLOADER_H

#include <QObject>
#include <QByteArray>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

/**
 * Runs in a worker thread. Performs HTTPS GET with progressive (chunked) delivery
 * so the main thread is never blocked by DNS/SSL/socket.
 *
 * Emits chunkReady() per readyRead, then progressiveDownloadFinished().
 */
class StreamDownloader : public QObject
{
    Q_OBJECT

public:
    explicit StreamDownloader(QObject* parent = nullptr);
    ~StreamDownloader();

public slots:
    void startProgressiveDownload(const QString& url, const QString& trackId);

signals:
    void chunkReady(const QByteArray& chunk, const QString& trackId);
    void progressiveDownloadFinished(const QString& errorMessage, const QString& trackId);

private slots:
    void onReadyRead();
    void onProgressiveReplyFinished();

private:
    QNetworkAccessManager* m_nam;
    QNetworkReply* m_reply;
};

#endif // STREAMDOWNLOADER_H
