#include "discordmanager.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDataStream>
#include <QCoreApplication>
#include <QTimer>
#include <QDebug>

DiscordManager::DiscordManager(QObject *parent)
    : QObject(parent)
    , m_socket(nullptr)
    , m_reconnectTimer(nullptr)
    , m_enabled(true)
    , m_connected(false)
    , m_isPlaying(false)
    , m_positionSeconds(0)
    , m_pipeIndex(0)
{
}

DiscordManager::~DiscordManager()
{
}

void DiscordManager::start(const QString& clientId)
{
    m_clientId = clientId;
    
    if (!m_socket) {
        m_socket = new QLocalSocket(this);
        connect(m_socket, &QLocalSocket::connected, this, &DiscordManager::onConnected);
        connect(m_socket, &QLocalSocket::disconnected, this, &DiscordManager::onDisconnected);
        connect(m_socket, QOverload<QLocalSocket::LocalSocketError>::of(&QLocalSocket::errorOccurred), this, &DiscordManager::onError);
        connect(m_socket, &QLocalSocket::readyRead, this, &DiscordManager::onReadyRead);
    }
    
    if (!m_reconnectTimer) {
        m_reconnectTimer = new QTimer(this);
        m_reconnectTimer->setInterval(5000);
        connect(m_reconnectTimer, &QTimer::timeout, this, &DiscordManager::connectToDiscord);
    }
    
    if (m_enabled) {
        m_reconnectTimer->start();
        connectToDiscord();
    }
}

void DiscordManager::stop()
{
    if (m_reconnectTimer) m_reconnectTimer->stop();
    if (m_socket && m_socket->isOpen()) {
        m_socket->close();
    }
    m_connected = false;
}

void DiscordManager::setEnabled(bool enabled)
{
    if (m_enabled == enabled) return;
    m_enabled = enabled;
    
    if (m_enabled) {
        if (!m_connected) m_reconnectTimer->start();
        connectToDiscord();
    } else {
        m_reconnectTimer->stop();
        m_connected = false;
        if (m_socket->isOpen()) {
            m_socket->close();
        }
    }
}

void DiscordManager::updatePresence(std::shared_ptr<Track> track, bool isPlaying, int positionSeconds)
{
    bool changed = false;
    if (!m_currentTrack || !track || m_currentTrack->id() != track->id()) changed = true;
    if (m_isPlaying != isPlaying) changed = true;
    // We don't check positionSeconds here to avoid updating every second
    
    m_currentTrack = track;
    m_isPlaying = isPlaying;
    m_positionSeconds = positionSeconds;
    
    if (!m_enabled) return;
    
    if (!m_connected) {
        if (!m_reconnectTimer->isActive()) {
            connectToDiscord();
            m_reconnectTimer->start();
        }
        return;
    }
    
    if (changed) {
        buildAndSendActivity();
    }
}

void DiscordManager::connectToDiscord()
{
    if (!m_enabled || m_connected || m_socket->state() != QLocalSocket::UnconnectedState) return;
    
    QString pipeName;
#ifdef Q_OS_WIN
    pipeName = QString("\\\\.\\pipe\\discord-ipc-%1").arg(m_pipeIndex);
#else
    pipeName = QString("discord-ipc-%1").arg(m_pipeIndex);
#endif

    emit debugLog(QString("[Discord] Attempting connection to %1").arg(pipeName));
    m_socket->connectToServer(pipeName);
}

void DiscordManager::onConnected()
{
    emit debugLog(QString("[Discord] Connected to pipe %1").arg(m_pipeIndex));
    m_connected = true;
    m_reconnectTimer->stop();
    sendHandshakeStatus();
    // We wait for READY event in onReadyRead before sending activity
}

void DiscordManager::onDisconnected()
{
    emit debugLog("[Discord] Disconnected");
    m_connected = false;
    if (m_enabled) m_reconnectTimer->start();
}

void DiscordManager::onError(QLocalSocket::LocalSocketError socketError)
{
    QString err = m_socket->errorString();
    emit debugLog(QString("[Discord] Socket error: %1").arg(err));
    m_connected = false;
    // Rotate pipe index for next attempt
    m_pipeIndex = (m_pipeIndex + 1) % 10;
}

void DiscordManager::onReadyRead()
{
    QByteArray data = m_socket->readAll();
    if (data.size() < 8) return;
    
    // Process potentially multiple messages in the buffer
    int offset = 0;
    while (offset + 8 <= data.size()) {
        // Opcode (first 4 bytes, Little Endian)
        uint32_t opcode = static_cast<uint8_t>(data[offset]) |
                         (static_cast<uint8_t>(data[offset + 1]) << 8) |
                         (static_cast<uint8_t>(data[offset + 2]) << 16) |
                         (static_cast<uint8_t>(data[offset + 3]) << 24);
        
        // Length (next 4 bytes, Little Endian)
        uint32_t length = static_cast<uint8_t>(data[offset + 4]) |
                         (static_cast<uint8_t>(data[offset + 5]) << 8) |
                         (static_cast<uint8_t>(data[offset + 6]) << 16) |
                         (static_cast<uint8_t>(data[offset + 7]) << 24);
        
        if (offset + 8 + length > data.size()) break; // Incomplete message
        
        QByteArray payloadData = data.mid(offset + 8, length);
        offset += 8 + length;
        
        QJsonDocument doc = QJsonDocument::fromJson(payloadData);
        if (doc.isNull()) continue;
        
        QJsonObject response = doc.object();
        QString jsonStr = doc.toJson(QJsonDocument::Compact);
        
        if (opcode == 2) {
            QString message = response["message"].toString();
            emit debugLog(QString("[Discord] Connection closed by server: %1").arg(message));
            m_socket->close();
            return;
        }

        //emit debugLog(QString("[Discord] Received Opcode %1: %2").arg(opcode).arg(jsonStr));
        
        // Check for READY event
        if (response.contains("evt") && response["evt"].toString() == "READY") {
            emit debugLog("[Discord] Ready received, sending initial activity");
            if (m_currentTrack) {
                buildAndSendActivity();
            }
        }
    }
}

void DiscordManager::sendHandshakeStatus()
{
    QJsonObject payload;
    payload["v"] = 1;
    payload["client_id"] = m_clientId;
    
    sendPayload(0, payload); // Opcode 0 = Handshake
}

void DiscordManager::sendPayload(int opcode, const QJsonObject& payload)
{
    if (!m_socket->isOpen()) return;
    
    QByteArray jsonData = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    
    // Discord IPC header: 4 bytes opcode (LE), 4 bytes length (LE)
    QByteArray header;
    header.resize(8);
    
    // Use little endian
    header[0] = static_cast<char>(opcode & 0xFF);
    header[1] = static_cast<char>((opcode >> 8) & 0xFF);
    header[2] = static_cast<char>((opcode >> 16) & 0xFF);
    header[3] = static_cast<char>((opcode >> 24) & 0xFF);
    
    int length = jsonData.size();
    header[4] = static_cast<char>(length & 0xFF);
    header[5] = static_cast<char>((length >> 8) & 0xFF);
    header[6] = static_cast<char>((length >> 16) & 0xFF);
    header[7] = static_cast<char>((length >> 24) & 0xFF);
    
    m_socket->write(header);
    m_socket->write(jsonData);
    // m_socket->flush(); // Removed flush() to avoid potential blocking
}

void DiscordManager::buildAndSendActivity()
{
    QJsonObject activity;
    if (m_currentTrack && m_isPlaying) {
        activity["type"] = 2; // Listening
        activity["details"] = m_currentTrack->title();
        activity["state"] = m_currentTrack->artist();
        activity["status_display_type"] = 1; // 1 = Show 'state' in the primary status line
        
        QJsonObject assets;
        QString artUrl = m_currentTrack->albumArt();
        if (!artUrl.isEmpty()) {
            assets["large_image"] = artUrl;
            assets["large_text"] = m_currentTrack->album();
            activity["assets"] = assets;
        }
        
        QJsonObject timestamps;
        qint64 now = QDateTime::currentSecsSinceEpoch();
        timestamps["start"] = (double)(now - m_positionSeconds);
        activity["timestamps"] = timestamps;
    } else {
        // Leave activity as an empty object or send null to clear
    }

    QJsonObject args;
    args["pid"] = static_cast<double>(QCoreApplication::applicationPid()); // Use double for JSON safety
    if (activity.isEmpty()) {
        args["activity"] = QJsonValue::Null;
    } else {
        args["activity"] = activity;
    }

    QJsonObject payload;
    payload["cmd"] = "SET_ACTIVITY";
    payload["args"] = args;
    payload["nonce"] = QString::number(QDateTime::currentMSecsSinceEpoch());

    sendPayload(1, payload); // Opcode 1 = Frame
    emit debugLog(QString("[Discord] Activity updated: %1").arg(m_currentTrack ? m_currentTrack->title() : "None"));
}
