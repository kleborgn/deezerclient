#ifndef DISCORDMANAGER_H
#define DISCORDMANAGER_H

#include <QObject>
#include <QLocalSocket>
#include <QDateTime>
#include <memory>
#include "track.h"

class DiscordManager : public QObject
{
    Q_OBJECT
public:
    explicit DiscordManager(QObject *parent = nullptr);
    ~DiscordManager();

    void start(const QString& clientId);
    void stop();

    bool isEnabled() const { return m_enabled; }

public slots:
    void setEnabled(bool enabled);
    void updatePresence(std::shared_ptr<Track> track, bool isPlaying, int positionSeconds = 0);

signals:
    void debugLog(const QString& message);

private slots:
    void onConnected();
    void onDisconnected();
    void onError(QLocalSocket::LocalSocketError socketError);
    void onReadyRead();

private:
    void connectToDiscord();
    void sendPayload(int opcode, const QJsonObject& payload);
    void sendHandshakeStatus();
    void buildAndSendActivity();

    QString m_clientId;
    QLocalSocket* m_socket;
    class QTimer* m_reconnectTimer;
    bool m_enabled;
    bool m_connected;
    
    std::shared_ptr<Track> m_currentTrack;
    bool m_isPlaying;
    int m_positionSeconds;
    QString m_lastTrackId;
    bool m_lastPlayingState;
    
    int m_pipeIndex;
};

#endif // DISCORDMANAGER_H
