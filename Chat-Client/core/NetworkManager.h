#pragma once

#include <QObject>
#include <QTcpSocket>
#include <QTimer>

#include "protocol/PacketCodec.h"

class NetworkManager : public QObject
{
    Q_OBJECT

public:
    enum class ConnectionState
    {
        Disconnected,
        Connecting,
        Connected,
        LoggingIn,
        Authenticated
    };

    explicit NetworkManager(QObject *parent = nullptr);
    void login(const QString &username, const QString &encryptedPassword);

signals:
    void loginSuccessful();
    void loginFailed(const QString &errorMessage);
    void connectionStateChanged(NetworkManager::ConnectionState state);

private slots:
    void onConnected();
    void onDisconnected();
    void onReadyRead();
    void onSocketError(QAbstractSocket::SocketError socketError);
    void sendHeartbeat();

private:
    void connectToServer();
    void sendLoginRequest();
    void handlePacket(const XYChat::Protocol::Packet &packet);
    void handleLoginResponse(const XYChat::Protocol::Packet &packet);
    void sendPacket(const XYChat::Protocol::Packet &packet);
    quint64 nextRequestId();
    void setState(ConnectionState state);

private:
    QTcpSocket *m_tcpSocket;
    QTimer *m_heartbeatTimer;
    QTimer *m_reconnectTimer;
    XYChat::Protocol::PacketCodec m_codec;
    ConnectionState m_state = ConnectionState::Disconnected;
    quint64 m_nextRequestId = 1;
    quint64 m_pendingLoginRequestId = 0;
    QString m_pendingUsername;
    QString m_pendingEncryptedPassword;
    bool m_loginQueued = false;
    bool m_reconnectEnabled = false;
};
