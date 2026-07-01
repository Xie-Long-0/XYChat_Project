#pragma once

#include <QByteArray>
#include <QtGlobal>

namespace XYChat::Protocol
{
inline constexpr quint32 Magic = 0x58594350; // "XYCP"
inline constexpr quint16 CurrentVersion = 1;
inline constexpr quint32 HeaderSize = sizeof(quint32) + sizeof(quint16) + sizeof(quint16)
    + sizeof(quint64) + sizeof(quint32);
inline constexpr quint32 MaxPayloadSize = 4 * 1024 * 1024;

enum class MessageType : quint16
{
    LoginRequest = 1,
    LoginResponse = 2,
    Ping = 3,
    Pong = 4,
    Error = 5
};

enum class ErrorCode : int
{
    Ok = 0,
    InvalidRequest = 1000,
    UnsupportedVersion = 1001,
    AuthenticationFailed = 2001,
    Timeout = 9001,
    InternalError = 9002
};

struct Packet
{
    quint16 version = CurrentVersion;
    MessageType messageType = MessageType::Error;
    quint64 requestId = 0;
    QByteArray payload;
};
}
