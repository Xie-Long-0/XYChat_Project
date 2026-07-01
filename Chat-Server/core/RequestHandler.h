#pragma once

#include <QThread>
#include <QTcpSocket>
#include <QJsonObject>
#include <QTimer>

#include "protocol/PacketCodec.h"

class RequestHandler : public QThread
{
    Q_OBJECT

public:
    explicit RequestHandler(qintptr socketDescriptor, QObject *parent = nullptr);
    void run() override;

signals:
    void finished();

private slots:
    void onReadyRead();
    void onIdleTimeout();

private:
    void processPacket(const XYChat::Protocol::Packet &packet);
    void processLoginRequest(const XYChat::Protocol::Packet &packet, const QJsonObject &request);
    void sendResponse(quint64 requestId,
                      XYChat::Protocol::MessageType messageType,
                      XYChat::Protocol::ErrorCode code,
                      const QString &message,
                      const QJsonObject &data = {});
    void sendPacket(const XYChat::Protocol::Packet &packet);

private:
    QTcpSocket *m_socket = nullptr;
    QTimer *m_idleTimer = nullptr;
    XYChat::Protocol::PacketCodec m_codec;
    qintptr m_socketDescriptor;
};
