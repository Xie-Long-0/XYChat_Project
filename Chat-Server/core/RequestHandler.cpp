#include "RequestHandler.h"
#include "database/DatabaseManager.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

using namespace XYChat::Protocol;

RequestHandler::RequestHandler(qintptr socketDescriptor, QObject *parent)
    : QThread(parent)
    , m_socketDescriptor(socketDescriptor)
{
}

void RequestHandler::run()
{
    m_socket = new QTcpSocket();
    if (!m_socket->setSocketDescriptor(m_socketDescriptor)) {
        qDebug() << "Failed to set socket descriptor";
        delete m_socket;
        m_socket = nullptr;
        emit finished();
        return;
    }

    m_idleTimer = new QTimer();
    m_idleTimer->setInterval(90000);
    m_idleTimer->setSingleShot(true);

    connect(m_socket, &QTcpSocket::readyRead, m_socket, [this]() { onReadyRead(); });
    connect(m_socket, &QTcpSocket::disconnected, m_socket, [this]() { quit(); });
    connect(m_idleTimer, &QTimer::timeout, m_idleTimer, [this]() { onIdleTimeout(); });
    m_idleTimer->start();

    exec();

    delete m_idleTimer;
    m_idleTimer = nullptr;
    delete m_socket;
    m_socket = nullptr;
}

void RequestHandler::onReadyRead()
{
    m_idleTimer->start();
    m_codec.appendData(m_socket->readAll());

    while (true) {
        Packet packet;
        QString errorMessage;
        const PacketCodec::DecodeStatus status = m_codec.nextPacket(packet, &errorMessage);
        if (status == PacketCodec::DecodeStatus::NeedMoreData) {
            return;
        }
        if (status == PacketCodec::DecodeStatus::InvalidData) {
            sendResponse(0, MessageType::Error, ErrorCode::InvalidRequest, errorMessage);
            m_socket->disconnectFromHost();
            return;
        }

        processPacket(packet);
    }
}

void RequestHandler::onIdleTimeout()
{
    sendResponse(0, MessageType::Error, ErrorCode::Timeout, QStringLiteral("Idle timeout"));
    m_socket->disconnectFromHost();
}

void RequestHandler::processPacket(const Packet &packet)
{
    if (packet.messageType == MessageType::Ping) {
        Packet pong;
        pong.messageType = MessageType::Pong;
        pong.requestId = packet.requestId;
        sendPacket(pong);
        return;
    }

    if (packet.messageType != MessageType::LoginRequest) {
        sendResponse(packet.requestId,
                     MessageType::Error,
                     ErrorCode::InvalidRequest,
                     QStringLiteral("Invalid request type"));
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument jsonDoc = QJsonDocument::fromJson(packet.payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !jsonDoc.isObject()) {
        sendResponse(packet.requestId,
                     MessageType::LoginResponse,
                     ErrorCode::InvalidRequest,
                     QStringLiteral("Invalid JSON payload"));
        return;
    }

    const QJsonObject json = jsonDoc.object();
    if (json.value(QStringLiteral("type")).toString() != QStringLiteral("login")) {
        sendResponse(packet.requestId,
                     MessageType::LoginResponse,
                     ErrorCode::InvalidRequest,
                     QStringLiteral("Invalid request type"));
        return;
    }

    processLoginRequest(packet, json);
}

void RequestHandler::processLoginRequest(const Packet &packet, const QJsonObject &request)
{
    const QString username = request.value(QStringLiteral("username")).toString();
    const QString encryptedPassword = request.value(QStringLiteral("password")).toString();

    qDebug() << m_socket->peerAddress().toString() << "login request" << packet.requestId << username;

    DatabaseManager dbManager;
    if (dbManager.verifyUser(username, encryptedPassword)) {
        QJsonObject data;
        data[QStringLiteral("username")] = username;
        sendResponse(packet.requestId,
                     MessageType::LoginResponse,
                     ErrorCode::Ok,
                     QStringLiteral("OK"),
                     data);
        qDebug() << username << "login successful";
        return;
    }

    sendResponse(packet.requestId,
                 MessageType::LoginResponse,
                 ErrorCode::AuthenticationFailed,
                 QStringLiteral("Invalid username or password"));
    qDebug() << username << "login failed";
}

void RequestHandler::sendResponse(quint64 requestId,
                                  MessageType messageType,
                                  ErrorCode code,
                                  const QString &message,
                                  const QJsonObject &data)
{
    QJsonObject response;
    response[QStringLiteral("code")] = static_cast<int>(code);
    response[QStringLiteral("message")] = message;
    response[QStringLiteral("data")] = data;

    Packet packet;
    packet.messageType = messageType;
    packet.requestId = requestId;
    packet.payload = QJsonDocument(response).toJson(QJsonDocument::Compact);
    sendPacket(packet);
}

void RequestHandler::sendPacket(const Packet &packet)
{
    const QByteArray encoded = PacketCodec::encode(packet);
    if (encoded.isEmpty()) {
        qWarning() << "Failed to encode packet" << packet.requestId;
        return;
    }

    m_socket->write(encoded);
    m_socket->flush();
}
