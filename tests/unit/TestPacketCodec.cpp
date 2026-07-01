#include <QtTest/QtTest>

#include "protocol/PacketCodec.h"

using namespace XYChat::Protocol;

class TestPacketCodec : public QObject
{
    Q_OBJECT

private slots:
    void roundTripsPacketHeaderAndPayload();
    void parsesManyConsecutiveSmallPackets();
    void waitsForSplitLargePacket();
};

void TestPacketCodec::roundTripsPacketHeaderAndPayload()
{
    PacketCodec codec;
    Packet original;
    original.messageType = MessageType::LoginRequest;
    original.requestId = 42;
    original.payload = R"({"type":"login"})";

    codec.appendData(PacketCodec::encode(original));

    Packet decoded;
    QCOMPARE(codec.nextPacket(decoded), PacketCodec::DecodeStatus::PacketReady);
    QCOMPARE(decoded.version, CurrentVersion);
    QCOMPARE(decoded.messageType, MessageType::LoginRequest);
    QCOMPARE(decoded.requestId, quint64(42));
    QCOMPARE(decoded.payload, original.payload);
}

void TestPacketCodec::parsesManyConsecutiveSmallPackets()
{
    PacketCodec codec;
    QByteArray stream;

    for (quint64 i = 1; i <= 1000; ++i) {
        Packet packet;
        packet.messageType = MessageType::Ping;
        packet.requestId = i;
        packet.payload = QByteArray::number(static_cast<qint64>(i));
        stream.append(PacketCodec::encode(packet));
    }

    codec.appendData(stream);

    for (quint64 i = 1; i <= 1000; ++i) {
        Packet decoded;
        QCOMPARE(codec.nextPacket(decoded), PacketCodec::DecodeStatus::PacketReady);
        QCOMPARE(decoded.messageType, MessageType::Ping);
        QCOMPARE(decoded.requestId, i);
        QCOMPARE(decoded.payload, QByteArray::number(static_cast<qint64>(i)));
    }

    Packet decoded;
    QCOMPARE(codec.nextPacket(decoded), PacketCodec::DecodeStatus::NeedMoreData);
    QCOMPARE(codec.bufferedSize(), qsizetype(0));
}

void TestPacketCodec::waitsForSplitLargePacket()
{
    PacketCodec codec;
    Packet original;
    original.messageType = MessageType::LoginResponse;
    original.requestId = 77;
    original.payload = QByteArray(128 * 1024, 'x');

    const QByteArray encoded = PacketCodec::encode(original);
    codec.appendData(encoded.left(HeaderSize + 123));

    Packet decoded;
    QCOMPARE(codec.nextPacket(decoded), PacketCodec::DecodeStatus::NeedMoreData);

    codec.appendData(encoded.mid(HeaderSize + 123));
    QCOMPARE(codec.nextPacket(decoded), PacketCodec::DecodeStatus::PacketReady);
    QCOMPARE(decoded.messageType, MessageType::LoginResponse);
    QCOMPARE(decoded.requestId, quint64(77));
    QCOMPARE(decoded.payload, original.payload);
}

QTEST_MAIN(TestPacketCodec)
#include "TestPacketCodec.moc"
