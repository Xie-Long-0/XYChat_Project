#pragma once

#include "Packet.h"

#include <QByteArray>
#include <QString>

namespace XYChat::Protocol
{
class PacketCodec
{
public:
    enum class DecodeStatus
    {
        NeedMoreData,
        PacketReady,
        InvalidData
    };

    static QByteArray encode(const Packet &packet);

    void appendData(const QByteArray &data);
    DecodeStatus nextPacket(Packet &packet, QString *errorMessage = nullptr);
    void reset();
    qsizetype bufferedSize() const;

private:
    QByteArray m_buffer;
};
}
