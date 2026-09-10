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
    // M1
    LoginRequest = 1,
    LoginResponse = 2,
    Ping = 3,
    Pong = 4,
    Error = 5,
    // M2
    RegisterRequest = 10,
    RegisterResponse = 11,
    LogoutRequest = 12,
    LogoutResponse = 13,
    TokenRenewRequest = 14,
    TokenRenewResponse = 15,
    ForceLogoutRequest = 16,
    ForceLogoutResponse = 17,
    // M3 - 用户搜索与联系人
    SearchUsersRequest = 20,
    SearchUsersResponse = 21,
    AddContactRequest = 22,
    AddContactResponse = 23,
    GetContactsRequest = 24,
    GetContactsResponse = 25,
    // M3 - 会话与消息
    GetConversationsRequest = 30,
    GetConversationsResponse = 31,
    SendMessageRequest = 32,
    SendMessageResponse = 33,
    NewMessageNotification = 34,
    AckMessageRequest = 35,
    AckMessageResponse = 36,
    SyncMessagesRequest = 37,
    SyncMessagesResponse = 38,
    MessageStatusUpdate = 39,
    // M5.5 - 账号级增量同步
    SyncEventsRequest = 40,
    SyncEventsResponse = 41,
    // M6 - 端到端加密密钥交换
    RegisterKeysRequest = 50,
    RegisterKeysResponse = 51,
    FetchKeysRequest = 52,
    FetchKeysResponse = 53,
    // M7a - 明文群聊
    CreateGroupRequest = 60,
    CreateGroupResponse = 61,
    InviteGroupMembersRequest = 62,
    InviteGroupMembersResponse = 63,
    LeaveGroupRequest = 64,
    LeaveGroupResponse = 65,
    KickGroupMemberRequest = 66,
    KickGroupMemberResponse = 67,
    GetGroupInfoRequest = 68,
    GetGroupInfoResponse = 69,
    GroupChangedNotification = 70, // 服务端推送：群成员变更/系统消息通知
    // M7b - 群聊端到端加密
    FetchGroupKeysRequest = 71,
    FetchGroupKeysResponse = 72,
    // M9 - 多端同步与离线一致性
    ReadCursorNotification = 80, // 服务端推送：已读者自身读游标更新（同账号多端已读同步）
    // M9 特性栈：会话置顶/免打扰
    SetConversationPrefsRequest = 81,
    SetConversationPrefsResponse = 82,
    ConversationPrefsNotification = 83, // 服务端推送：本人会话偏好变更（多端同步）
    // M9 特性栈：消息编辑/删除
    EditMessageRequest = 84,
    EditMessageResponse = 85,
    DeleteMessageRequest = 86,
    DeleteMessageResponse = 87,
    // M9 欠账修复：编辑/删除事件专用推送类型（与 ReadCursorNotification(80)/
    // ConversationPrefsNotification(83) 风格统一，不再靠 requestId==0 区分响应与推送）
    MessageEditedNotification = 88,  // 服务端推送：会话成员编辑消息（多端实时一致）
    MessageDeletedNotification = 89, // 服务端推送：会话成员删除消息（多端实时一致）
};

enum class ErrorCode : int
{
    Ok = 0,
    // 1xxx: 请求相关
    InvalidRequest = 1000,
    UnsupportedVersion = 1001,
    ReplayRejected = 1002,
    RateLimited = 1003,            // 非登录类请求频率超限（发消息/搜索/密钥拉取）
    // 2xxx: 认证相关
    AuthenticationFailed = 2001,
    AccountAlreadyExists = 2002,
    AccountNotFound = 2003,
    SessionExpired = 2004,
    SessionInvalid = 2005,
    LoginRateLimited = 2006,
    TooManyDevices = 2007,
    // 3xxx: 联系人/会话相关
    ContactAlreadyExists = 3001,
    ContactNotFound = 3002,
    ConversationNotFound = 3003,
    MessageNotFound = 3004,
    CannotSendToSelf = 3005,
    PermissionDenied = 3006,
    // M6: 端到端加密相关
    KeyBundleUnavailable = 3007,   // 对方无可用设备或预密钥耗尽
    E2eeInvalidEnvelope = 3008,    // 消息密文 envelope 非法
    // M7a: 群组相关
    GroupLimitExceeded = 3009,     // 群数量或成员数超限
    MemberAlreadyExists = 3010,    // 被邀请者已在群中
    MemberNotFound = 3011,         // 目标不是群成员
    NotGroupOwner = 3012,          // 仅群主可执行的管理操作
    // 9xxx: 系统相关
    Timeout = 9001,
    InternalError = 9002,
};

struct Packet
{
    quint16 version = CurrentVersion;
    MessageType messageType = MessageType::Error;
    quint64 requestId = 0;
    QByteArray payload;
};
}
