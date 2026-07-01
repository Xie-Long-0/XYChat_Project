#include "PacketCodec.h"

#include <QDataStream>
#include <QIODevice>

namespace XYChat::Protocol
{
QByteArray PacketCodec::encode(const Packet &packet)
{
    if (packet.payload.size() > MaxPayloadSize) {
        return {};
    }

    QByteArray data;
    data.reserve(HeaderSize + packet.payload.size());

    QDataStream stream(&data, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << Magic;
    stream << packet.version;
    stream << static_cast<quint16>(packet.messageType);
    stream << packet.requestId;
    stream << static_cast<quint32>(packet.payload.size());
    data.append(packet.payload);

    return data;
}

void PacketCodec::appendData(const QByteArray &data)
{
    m_buffer.append(data);
}

PacketCodec::DecodeStatus PacketCodec::nextPacket(Packet &packet, QString *errorMessage)
{
    if (m_buffer.size() < static_cast<qsizetype>(HeaderSize)) {
        return DecodeStatus::NeedMoreData;
    }

    QDataStream stream(m_buffer);
    stream.setByteOrder(QDataStream::BigEndian);

    quint32 magic = 0;
    quint16 version = 0;
    quint16 messageType = 0;
    quint64 requestId = 0;
    quint32 payloadLength = 0;
    stream >> magic >> version >> messageType >> requestId >> payloadLength;

    if (magic != Magic) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Invalid packet magic");
        }
        m_buffer.clear();
        return DecodeStatus::InvalidData;
    }

    if (version != CurrentVersion) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Unsupported protocol version");
        }
        m_buffer.clear();
        return DecodeStatus::InvalidData;
    }

    if (payloadLength > MaxPayloadSize) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Payload too large");
        }
        m_buffer.clear();
        return DecodeStatus::InvalidData;
    }

    const qsizetype packetLength = static_cast<qsizetype>(HeaderSize + payloadLength);
    if (m_buffer.size() < packetLength) {
        return DecodeStatus::NeedMoreData;
    }

    packet.version = version;
    packet.messageType = static_cast<MessageType>(messageType);
    packet.requestId = requestId;
    packet.payload = m_buffer.mid(HeaderSize, payloadLength);
    m_buffer.remove(0, packetLength);

    return DecodeStatus::PacketReady;
}

void PacketCodec::reset()
{
    m_buffer.clear();
}

qsizetype PacketCodec::bufferedSize() const
{
    return m_buffer.size();
}
}
