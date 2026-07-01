#include "NetworkManager.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSysInfo>

using namespace XYChat::Protocol;

NetworkManager::NetworkManager(QObject *parent) :
    QObject(parent),
    m_tcpSocket(new QTcpSocket(this)),
    m_heartbeatTimer(new QTimer(this)),
    m_reconnectTimer(new QTimer(this))
{
    m_heartbeatTimer->setInterval(30000);
    m_reconnectTimer->setInterval(3000);
    m_reconnectTimer->setSingleShot(true);

    connect(m_tcpSocket, &QTcpSocket::connected, this, &NetworkManager::onConnected);
    connect(m_tcpSocket, &QTcpSocket::disconnected, this, &NetworkManager::onDisconnected);
    connect(m_tcpSocket, &QTcpSocket::readyRead, this, &NetworkManager::onReadyRead);
    connect(m_tcpSocket, &QTcpSocket::errorOccurred, this, &NetworkManager::onSocketError);
    connect(m_heartbeatTimer, &QTimer::timeout, this, &NetworkManager::sendHeartbeat);
    connect(m_reconnectTimer, &QTimer::timeout, this, &NetworkManager::connectToServer);
}

void NetworkManager::login(const QString &username, const QString &encryptedPassword)
{
    m_pendingUsername = username;
    m_pendingEncryptedPassword = encryptedPassword;
    m_loginQueued = true;
    m_reconnectEnabled = true;

    if (m_state == ConnectionState::Connected || m_state == ConnectionState::Authenticated) {
        sendLoginRequest();
        return;
    }

    if (m_state == ConnectionState::Disconnected) {
        connectToServer();
    }
}

void NetworkManager::onConnected()
{
    setState(ConnectionState::Connected);
    m_heartbeatTimer->start();

    if (m_loginQueued) {
        sendLoginRequest();
    }
}

void NetworkManager::onDisconnected()
{
    const bool shouldRelogin = m_reconnectEnabled && !m_pendingUsername.isEmpty();

    m_heartbeatTimer->stop();
    m_codec.reset();
    m_pendingLoginRequestId = 0;
    setState(ConnectionState::Disconnected);

    if (shouldRelogin) {
        m_loginQueued = true;
        m_reconnectTimer->start();
    }
}

void NetworkManager::onReadyRead()
{
    m_codec.appendData(m_tcpSocket->readAll());

    while (true) {
        Packet packet;
        QString errorMessage;
        const PacketCodec::DecodeStatus status = m_codec.nextPacket(packet, &errorMessage);
        if (status == PacketCodec::DecodeStatus::NeedMoreData) {
            return;
        }
        if (status == PacketCodec::DecodeStatus::InvalidData) {
            emit loginFailed(errorMessage);
            m_tcpSocket->disconnectFromHost();
            return;
        }

        handlePacket(packet);
    }
}

void NetworkManager::onSocketError(QAbstractSocket::SocketError socketError)
{
    Q_UNUSED(socketError);

    if (m_state == ConnectionState::Connecting || m_state == ConnectionState::LoggingIn) {
        emit loginFailed(m_tcpSocket->errorString());
    }
}

void NetworkManager::sendHeartbeat()
{
    if (m_state == ConnectionState::Disconnected || m_state == ConnectionState::Connecting) {
        return;
    }

    Packet packet;
    packet.messageType = MessageType::Ping;
    packet.requestId = nextRequestId();
    sendPacket(packet);
}

void NetworkManager::connectToServer()
{
    if (m_state != ConnectionState::Disconnected) {
        return;
    }

    setState(ConnectionState::Connecting);
    m_tcpSocket->connectToHost(QStringLiteral("127.0.0.1"), 12345);
}

void NetworkManager::sendLoginRequest()
{
    QJsonObject json;
    json["type"] = QStringLiteral("login");
    json["username"] = m_pendingUsername;
    json["password"] = m_pendingEncryptedPassword;
    json["clientVersion"] = QStringLiteral("0.1.0");
    json["platform"] = QSysInfo::productType();
    json["deviceId"] = QString::fromLatin1(QSysInfo::machineUniqueId().toHex());

    Packet packet;
    packet.messageType = MessageType::LoginRequest;
    packet.requestId = nextRequestId();
    packet.payload = QJsonDocument(json).toJson(QJsonDocument::Compact);

    m_pendingLoginRequestId = packet.requestId;
    setState(ConnectionState::LoggingIn);
    sendPacket(packet);
}

void NetworkManager::handlePacket(const Packet &packet)
{
    if (packet.messageType == MessageType::LoginResponse) {
        handleLoginResponse(packet);
        return;
    }

    if (packet.messageType == MessageType::Ping) {
        Packet pong;
        pong.messageType = MessageType::Pong;
        pong.requestId = packet.requestId;
        sendPacket(pong);
    }
}

void NetworkManager::handleLoginResponse(const Packet &packet)
{
    if (packet.requestId != m_pendingLoginRequestId) {
        return;
    }

    const QJsonDocument responseDoc = QJsonDocument::fromJson(packet.payload);
    const QJsonObject response = responseDoc.object();
    const int code = response.value(QStringLiteral("code")).toInt(static_cast<int>(ErrorCode::InternalError));

    m_pendingLoginRequestId = 0;
    if (code == static_cast<int>(ErrorCode::Ok)) {
        m_loginQueued = false;
        setState(ConnectionState::Authenticated);
        emit loginSuccessful();
        return;
    }

    setState(ConnectionState::Connected);
    const QString message = response.value(QStringLiteral("message")).toString(QStringLiteral("Unknown error"));
    emit loginFailed(message);
}

void NetworkManager::sendPacket(const Packet &packet)
{
    const QByteArray encoded = PacketCodec::encode(packet);
    if (encoded.isEmpty()) {
        emit loginFailed(QStringLiteral("Failed to encode request"));
        return;
    }

    m_tcpSocket->write(encoded);
}

quint64 NetworkManager::nextRequestId()
{
    return m_nextRequestId++;
}

void NetworkManager::setState(ConnectionState state)
{
    if (m_state == state) {
        return;
    }

    m_state = state;
    emit connectionStateChanged(m_state);
}
