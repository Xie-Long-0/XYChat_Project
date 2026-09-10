#include "RequestHandler.h"
#include "NonceCache.h"
#include "database/DatabaseManager.h"
#include "E2eeCrypto.h"
#include "EncryptionManager.h"
#include "GroupE2eeCrypto.h"
#include "LogSanitizer.h"
#include "SecureMemory.h"
#include "StructuredLogger.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonParseError>
#include <QDateTime>
#include <QHash>
#include <QSet>
#include <QUuid>
#include <QMetaObject>

using namespace XYChat::Protocol;
using XYChat::Security::LogSanitizer;
using XYChat::Security::GroupE2eeCrypto;
using XYChat::Security::StructuredLogger;
using XYChat::Security::LogLevel;

// 构造 / 析构
RequestHandler::RequestHandler(qintptr socketDescriptor, QObject *parent)
    : QThread(parent)
    , m_socketDescriptor(socketDescriptor)
    , m_fetchKeysWindow(MaxFetchKeysPerWindow, FetchKeysWindowSeconds)
    , m_sendWindow(MaxSendMessagesPerWindow, SendMessageWindowSeconds)
    , m_searchWindow(MaxSearchesPerWindow, SearchWindowSeconds)
    , m_editDeleteWindow(MaxEditDeletePerWindow, EditDeleteWindowSeconds)
    , m_prefsWindow(MaxPrefsPerWindow, PrefsWindowSeconds)
{
}

RequestHandler::~RequestHandler()
{
    delete m_db;
    m_db = nullptr;
}

// M3: 跨线程发送数据到客户端
void RequestHandler::sendRawData(const QByteArray &data)
{
    // M5.5: 投递到线程亲和于 handler 线程的发送代理对象，
    // 确保 QSslSocket 只在其所属线程被访问
    QObject *worker = m_sendWorker.load(std::memory_order_acquire);
    if (!worker) {
        return;
    }
    QMetaObject::invokeMethod(worker, [this, data]() {
        if (m_socket && m_socket->state() == QAbstractSocket::ConnectedState) {
            m_socket->write(data);
            m_socket->flush();
        }
    }, Qt::QueuedConnection);
}

// M5.5: 请求断开客户端连接（在被终止会话时由 Server 调用）
void RequestHandler::disconnectClient()
{
    QObject *worker = m_sendWorker.load(std::memory_order_acquire);
    if (!worker) {
        return;
    }
    QMetaObject::invokeMethod(worker, [this]() {
        if (m_socket) {
            m_socket->disconnectFromHost();
        }
    }, Qt::QueuedConnection);
}

// M5: 设置 TLS 配置
void RequestHandler::setSslConfiguration(const QSslConfiguration &config)
{
    m_sslConfig = config;
    m_tlsEnabled = true;
}

// M5.5: 设置全局 nonce 缓存
void RequestHandler::setNonceCache(NonceCache *cache)
{
    m_nonceCache = cache;
}

// 线程入口
void RequestHandler::run()
{
    // 每个线程创建独立数据库连接
    const QString connName = QString("handler_%1").arg(reinterpret_cast<quintptr>(this));
    m_db = new DatabaseManager(connName);
    if (!m_db->initialize()) {
        StructuredLogger::event(LogLevel::Critical, "handler.db_init_failed")
            .field("connection", connName).write();
        emit finished();
        return;
    }

    m_socket = new QSslSocket();
    if (!m_socket->setSocketDescriptor(m_socketDescriptor)) {
        StructuredLogger::event(LogLevel::Warning, "handler.socket_init_failed").write();
        delete m_socket;
        m_socket = nullptr;
        emit finished();
        return;
    }

    // M5: 如果启用 TLS，启动服务端加密
    if (m_tlsEnabled) {
        m_socket->setSslConfiguration(m_sslConfig);
        connect(m_socket, &QSslSocket::encrypted, m_socket, [this]() {
            qDebug() << "[Handler] TLS handshake completed";
        });
        connect(m_socket, &QSslSocket::sslErrors, m_socket, [this](const QList<QSslError> &errors) {
            for (const auto &err : errors) {
                StructuredLogger::event(LogLevel::Warning, "connection.ssl_error")
                    .field("error", err.errorString()).write();
            }
            m_socket->disconnectFromHost();
        });
        m_socket->startServerEncryption();
    }

    m_idleTimer = new QTimer();
    m_idleTimer->setInterval(90000);
    m_idleTimer->setSingleShot(true);

    connect(m_socket, &QSslSocket::readyRead, m_socket, [this]() { onReadyRead(); });
    connect(m_socket, &QSslSocket::disconnected, m_socket, [this]() { quit(); });
    connect(m_idleTimer, &QTimer::timeout, m_idleTimer, [this]() { onIdleTimeout(); });
    m_idleTimer->start();

    // M5.5: 发送代理对象在当前（handler）线程创建，获得正确的线程亲和性
    QObject *worker = new QObject();
    m_sendWorker.store(worker, std::memory_order_release);

    exec();

    // 连接断开时，清理 session
    if (m_currentSessionId > 0) {
        emit userLoggedOut(m_authenticatedUserId, m_currentSessionId);
    }

    delete m_sendWorker.exchange(nullptr);
    delete m_idleTimer;
    m_idleTimer = nullptr;
    delete m_socket;
    m_socket = nullptr;
}

// 数据接收
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
            // M11: 服务器自发响应，显式标注类型，避免沿用上一个请求的陈旧 type
            m_currentRequestType = "invalid_frame";
            sendResponse(0, MessageType::Error, ErrorCode::InvalidRequest, errorMessage);
            m_socket->disconnectFromHost();
            return;
        }

        processPacket(packet);
    }
}

void RequestHandler::onIdleTimeout()
{
    // M11: 服务器自发响应，显式标注类型，避免中央审计日志沿用上一个请求的陈旧 type
    m_currentRequestType = "idle_timeout";
    sendResponse(0, MessageType::Error, ErrorCode::Timeout, "Idle timeout");
    m_socket->disconnectFromHost();
}

// 包分发
void RequestHandler::processPacket(const Packet &packet)
{
    // Ping/Pong 不需要认证
    if (packet.messageType == MessageType::Ping) {
        Packet pong;
        pong.messageType = MessageType::Pong;
        pong.requestId = packet.requestId;
        sendPacket(pong);
        return;
    }

    // M11: 记录每请求上下文（请求 ID/类型/起始计时），供 sendResponse 输出结构化审计日志
    m_currentRequestId = packet.requestId;
    m_currentRequestType.clear();
    m_requestTimer.restart();

    // 解析 JSON payload
    QJsonParseError parseError;
    const QJsonDocument jsonDoc = QJsonDocument::fromJson(packet.payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !jsonDoc.isObject()) {
        sendResponse(packet.requestId, MessageType::Error, ErrorCode::InvalidRequest,
                     "Invalid JSON payload");
        return;
    }
    const QJsonObject json = jsonDoc.object();
    const QString type = json.value("type").toString();
    m_currentRequestType = type;

    // M5: 重放保护检查（对所有业务请求）
    if (packet.messageType != MessageType::Ping && packet.messageType != MessageType::Pong) {
        if (!checkReplayProtection(json)) {
            sendResponse(packet.requestId, MessageType::Error, ErrorCode::ReplayRejected,
                         "Replay protection check failed");
            return;
        }
    }

    // 不需要认证的请求
    if (packet.messageType == MessageType::RegisterRequest || type == "register") {
        processRegisterRequest(packet, json);
        return;
    }
    if (packet.messageType == MessageType::LoginRequest || type == "login") {
        processLoginRequest(packet, json);
        return;
    }

    // 需要认证的请求：逐包验证 session token（P1 安全加固：回查 DB + 校验携带 token）
    if (!validateSession(json)) {
        sendResponse(packet.requestId, MessageType::Error, ErrorCode::SessionInvalid,
                     "Authentication required");
        return;
    }

    // 更新 session 活跃时间
    m_db->updateSessionLastActive(m_currentSessionId);

    if (packet.messageType == MessageType::LogoutRequest || type == "logout") {
        processLogoutRequest(packet);
        return;
    }
    if (packet.messageType == MessageType::TokenRenewRequest || type == "token_renew") {
        processTokenRenewRequest(packet, json);
        return;
    }
    // M5.5: 兼容旧字段名 force_logout 与新语义 terminate_session，
    // 均仅允许终止本人其他会话
    if (packet.messageType == MessageType::ForceLogoutRequest
        || type == "force_logout" || type == "terminate_session") {
        processTerminateSessionRequest(packet, json);
        return;
    }

    // M3 消息分发
    if (packet.messageType == MessageType::SearchUsersRequest || type == "search_users") {
        processSearchUsersRequest(packet, json);
        return;
    }
    if (packet.messageType == MessageType::AddContactRequest || type == "add_contact") {
        processAddContactRequest(packet, json);
        return;
    }
    if (packet.messageType == MessageType::GetContactsRequest || type == "get_contacts") {
        processGetContactsRequest(packet);
        return;
    }
    if (packet.messageType == MessageType::GetConversationsRequest || type == "get_conversations") {
        processGetConversationsRequest(packet);
        return;
    }
    if (packet.messageType == MessageType::SendMessageRequest || type == "send_message") {
        processSendMessageRequest(packet, json);
        return;
    }
    if (packet.messageType == MessageType::AckMessageRequest || type == "ack_message") {
        processAckMessageRequest(packet, json);
        return;
    }
    if (packet.messageType == MessageType::SyncMessagesRequest || type == "sync_messages") {
        processSyncMessagesRequest(packet, json);
        return;
    }
    // M5.5: 账号级增量同步
    if (packet.messageType == MessageType::SyncEventsRequest || type == "sync_events") {
        processSyncEventsRequest(packet, json);
        return;
    }
    // M6: 端到端加密密钥注册与拉取
    if (packet.messageType == MessageType::RegisterKeysRequest || type == "register_keys") {
        processRegisterKeysRequest(packet, json);
        return;
    }
    if (packet.messageType == MessageType::FetchKeysRequest || type == "fetch_keys") {
        processFetchKeysRequest(packet, json);
        return;
    }

    // M7a: 明文群聊
    if (packet.messageType == MessageType::CreateGroupRequest || type == "create_group") {
        processCreateGroupRequest(packet, json);
        return;
    }
    if (packet.messageType == MessageType::InviteGroupMembersRequest || type == "invite_group_members") {
        processInviteGroupMembersRequest(packet, json);
        return;
    }
    if (packet.messageType == MessageType::LeaveGroupRequest || type == "leave_group") {
        processLeaveGroupRequest(packet, json);
        return;
    }
    if (packet.messageType == MessageType::KickGroupMemberRequest || type == "kick_group_member") {
        processKickGroupMemberRequest(packet, json);
        return;
    }
    if (packet.messageType == MessageType::GetGroupInfoRequest || type == "get_group_info") {
        processGetGroupInfoRequest(packet, json);
        return;
    }
    if (packet.messageType == MessageType::FetchGroupKeysRequest || type == "fetch_group_keys") {
        processFetchGroupKeysRequest(packet, json);
        return;
    }
    // M9 特性栈：会话置顶/免打扰与消息编辑/删除
    if (packet.messageType == MessageType::SetConversationPrefsRequest
        || type == "set_conversation_prefs") {
        processSetConversationPrefsRequest(packet, json);
        return;
    }
    if (packet.messageType == MessageType::EditMessageRequest || type == "edit_message") {
        processEditMessageRequest(packet, json);
        return;
    }
    if (packet.messageType == MessageType::DeleteMessageRequest || type == "delete_message") {
        processDeleteMessageRequest(packet, json);
        return;
    }

    sendResponse(packet.requestId, MessageType::Error, ErrorCode::InvalidRequest,
                 QString("Unknown request type: %1").arg(type));
}

// 登录
void RequestHandler::processLoginRequest(const Packet &packet, const QJsonObject &request)
{
    const QString username = request.value("username").toString().trimmed();
    const QString clientPassword = request.value("password").toString();
    const QString deviceId = request.value("deviceId").toString();
    const QString clientVersion = request.value("clientVersion").toString();
    const QString platform = request.value("platform").toString();
    const QString ipAddr = m_socket->peerAddress().toString();

    if (username.isEmpty() || clientPassword.isEmpty()) {
        sendResponse(packet.requestId, MessageType::LoginResponse, ErrorCode::InvalidRequest,
                     "Username and password are required");
        return;
    }

    // 查找用户
    auto userOpt = m_db->getUserByUsername(username);
    if (!userOpt.has_value()) {
        m_db->recordLoginAttempt(0, ipAddr, false, "User not found");
        sendResponse(packet.requestId, MessageType::LoginResponse,
                     ErrorCode::AuthenticationFailed,
                     "Invalid username or password");
        return;
    }

    const UserInfo &user = userOpt.value();

    // 限流检查
    if (checkRateLimit(ipAddr, user.id)) {
        sendResponse(packet.requestId, MessageType::LoginResponse,
                     ErrorCode::LoginRateLimited,
                     "Too many failed login attempts. Please try again later.");
        return;
    }

    // 验证 PBKDF2 密码
    if (!EncryptionManager::verifyPassword(clientPassword, user.passwordHash)) {
        m_db->recordLoginAttempt(user.id, ipAddr, false, "Wrong password");
        sendResponse(packet.requestId, MessageType::LoginResponse,
                     ErrorCode::AuthenticationFailed,
                     "Invalid username or password");
        return;
    }

    // 生成 session token
    const QString token = EncryptionManager::generateToken();
    const QString tokenHash = EncryptionManager::hashToken(token);

    // 注册设备
    m_db->registerDevice(user.id, deviceId, QString("Device-%1").arg(deviceId.left(8)), platform);

    // 创建 session
    const qint64 sessionId = m_db->createSession(user.id, deviceId, tokenHash, ipAddr);
    if (sessionId < 0) {
        sendResponse(packet.requestId, MessageType::LoginResponse,
                     ErrorCode::InternalError, "Failed to create session");
        return;
    }

    // 记录成功审计
    m_db->recordLoginAttempt(user.id, ipAddr, true);

    // 更新认证状态
    m_authenticatedUserId = user.id;
    m_currentSessionId = sessionId;
    m_currentDeviceId = deviceId;

    // 返回响应
    QJsonObject data;
    data["username"] = user.username;
    data["userId"] = user.id;
    data["token"] = token;
    data["expiresAt"] = QDateTime::currentDateTimeUtc()
                                            .addSecs(86400 * 7)
                                            .toString(Qt::ISODate);

    emit userLoggedIn(user.id, sessionId, deviceId);

    sendResponse(packet.requestId, MessageType::LoginResponse, ErrorCode::Ok,
                 "OK", data);
}

// 注册
void RequestHandler::processRegisterRequest(const Packet &packet, const QJsonObject &request)
{
    const QString username = request.value("username").toString().trimmed();
    const QString password = request.value("password").toString();
    const QString email = request.value("email").toString().trimmed();
    const QString phone = request.value("phone").toString().trimmed();
    const QString ipAddr = m_socket->peerAddress().toString();

    // 输入校验
    if (username.isEmpty() || password.isEmpty()) {
        sendResponse(packet.requestId, MessageType::RegisterResponse,
                     ErrorCode::InvalidRequest,
                     "Username and password are required");
        return;
    }
    if (username.size() < 3 || username.size() > 32) {
        sendResponse(packet.requestId, MessageType::RegisterResponse,
                     ErrorCode::InvalidRequest,
                     "Username must be 3-32 characters");
        return;
    }
    if (password.size() < 6) {
        sendResponse(packet.requestId, MessageType::RegisterResponse,
                     ErrorCode::InvalidRequest,
                     "Password must be at least 6 characters");
        return;
    }

    // 检查用户名是否已存在
    if (m_db->userExists(username)) {
        sendResponse(packet.requestId, MessageType::RegisterResponse,
                     ErrorCode::AccountAlreadyExists,
                     "Username already exists");
        return;
    }

    // PBKDF2 哈希密码
    const QString passwordHash = EncryptionManager::hashPasswordWithSalt(password);

    // 创建用户
    const qint64 userId = m_db->registerUser(username, email, phone, passwordHash);
    if (userId < 0) {
        sendResponse(packet.requestId, MessageType::RegisterResponse,
                     ErrorCode::InternalError, "Failed to create account");
        return;
    }

    QJsonObject data;
    data["userId"] = userId;
    data["username"] = username;

    sendResponse(packet.requestId, MessageType::RegisterResponse, ErrorCode::Ok,
                 "Account created successfully", data);
    StructuredLogger::event(LogLevel::Info, "user.registered")
        .requestId(packet.requestId).userId(userId)
        .ipField("ip", ipAddr).write();
}

// 登出
void RequestHandler::processLogoutRequest(const Packet &packet)
{
    const qint64 sessionId = m_currentSessionId;
    const qint64 userId = m_authenticatedUserId;

    m_db->deleteSession(sessionId);
    emit userLoggedOut(userId, sessionId);

    QJsonObject data;
    sendResponse(packet.requestId, MessageType::LogoutResponse, ErrorCode::Ok,
                 "Logged out", data);

    // M11: 认证状态清零置于响应之后，使 sendResponse 的中央审计日志记录到实际登出用户
    // （userId 非 0）；登出事件另由 Server 的 session.offline 结构化日志覆盖。
    m_authenticatedUserId = 0;
    m_currentSessionId = 0;
}

// Token 续期
void RequestHandler::processTokenRenewRequest(const Packet &packet, const QJsonObject &request)
{
    const qint64 oldSessionId = m_currentSessionId;
    const qint64 userId = m_authenticatedUserId;

    // M5.5: 真正校验客户端携带的 token，而不是仅依赖连接内存状态
    const QString suppliedToken = request.value("token").toString();
    if (suppliedToken.isEmpty()) {
        sendResponse(packet.requestId, MessageType::TokenRenewResponse,
                     ErrorCode::InvalidRequest, "Token is required");
        return;
    }

    auto sessionOpt = m_db->getSessionById(oldSessionId);
    if (!sessionOpt.has_value()) {
        sendResponse(packet.requestId, MessageType::TokenRenewResponse,
                     ErrorCode::SessionExpired, "Session no longer exists");
        return;
    }

    const QString suppliedHash = EncryptionManager::hashToken(suppliedToken);
    if (suppliedHash != sessionOpt->tokenHash) {
        StructuredLogger::event(LogLevel::Warning, "token_renew.rejected")
            .requestId(packet.requestId).userId(userId)
            .field("reason", "token_mismatch").write();
        sendResponse(packet.requestId, MessageType::TokenRenewResponse,
                     ErrorCode::SessionInvalid, "Supplied token does not match session");
        return;
    }

    // 生成新 token
    const QString newToken = EncryptionManager::generateToken();
    const QString newTokenHash = EncryptionManager::hashToken(newToken);

    // 删除旧 session，创建新 session
    m_db->deleteSession(oldSessionId);

    const QString ipAddr = m_socket->peerAddress().toString();
    const qint64 newSessionId = m_db->createSession(userId, m_currentDeviceId, newTokenHash, ipAddr);
    if (newSessionId < 0) {
        sendResponse(packet.requestId, MessageType::TokenRenewResponse,
                     ErrorCode::InternalError, "Failed to renew token");
        return;
    }

    m_currentSessionId = newSessionId;

    QJsonObject data;
    data["token"] = newToken;
    data["expiresAt"] = QDateTime::currentDateTimeUtc()
                                            .addSecs(86400 * 7)
                                            .toString(Qt::ISODate);

    sendResponse(packet.requestId, MessageType::TokenRenewResponse, ErrorCode::Ok,
                 "Token renewed", data);
}

// 强制下线
void RequestHandler::processTerminateSessionRequest(const Packet &packet, const QJsonObject &request)
{
    // M5.5: 拒绝旧版越权用法：不允许指定任意 userId
    if (request.contains("userId")) {
        const qint64 requestedUserId = request.value("userId").toVariant().toLongLong();
        if (requestedUserId != m_authenticatedUserId) {
            sendResponse(packet.requestId, MessageType::ForceLogoutResponse,
                         ErrorCode::PermissionDenied,
                         "Cannot terminate sessions of another user");
            return;
        }
    }

    const qint64 targetSessionId = request.value("sessionId").toVariant().toLongLong();
    const QString targetDeviceId = request.value("deviceId").toString();

    if (targetSessionId <= 0 && targetDeviceId.isEmpty()) {
        sendResponse(packet.requestId, MessageType::ForceLogoutResponse,
                     ErrorCode::InvalidRequest,
                     "sessionId or deviceId is required");
        return;
    }
    if (targetSessionId == m_currentSessionId) {
        sendResponse(packet.requestId, MessageType::ForceLogoutResponse,
                     ErrorCode::InvalidRequest,
                     "Cannot terminate current session, use logout instead");
        return;
    }

    // 只能在本人会话列表中查找目标
    qint64 resolvedSessionId = 0;
    const auto sessions = m_db->getSessionsByUserId(m_authenticatedUserId);
    for (const auto &s : sessions) {
        if ((targetSessionId > 0 && s.id == targetSessionId)
            || (!targetDeviceId.isEmpty() && s.deviceId == targetDeviceId)) {
            resolvedSessionId = s.id;
            break;
        }
    }

    if (resolvedSessionId <= 0) {
        sendResponse(packet.requestId, MessageType::ForceLogoutResponse,
                     ErrorCode::PermissionDenied,
                     "Target session not found among your own sessions");
        return;
    }

    m_db->deleteSession(resolvedSessionId);
    emit userLoggedOut(m_authenticatedUserId, resolvedSessionId);
    emit sessionTerminated(resolvedSessionId);

    QJsonObject data;
    data["terminatedSessionId"] = resolvedSessionId;
    sendResponse(packet.requestId, MessageType::ForceLogoutResponse, ErrorCode::Ok,
                 "Session terminated", data);
    StructuredLogger::event(LogLevel::Info, "session.terminated")
        .requestId(packet.requestId).userId(m_authenticatedUserId)
        .field("terminatedSessionId", resolvedSessionId).write();
}

// M3: 用户搜索
void RequestHandler::processSearchUsersRequest(const Packet &packet, const QJsonObject &request)
{
    const QString query = request.value("query").toString().trimmed();
    if (query.isEmpty()) {
        sendResponse(packet.requestId, MessageType::SearchUsersResponse,
                     ErrorCode::InvalidRequest, "Query is required");
        return;
    }

    // M11: 搜索限流（连接级固定窗口），抑制用户名枚举/刷库；超限由结构化审计日志记录
    if (!m_searchWindow.allow(QDateTime::currentSecsSinceEpoch())) {
        sendResponse(packet.requestId, MessageType::SearchUsersResponse,
                     ErrorCode::RateLimited, "Too many search requests, please slow down");
        return;
    }

    auto users = m_db->searchUsers(query);

    QJsonArray userArray;
    for (const auto &u : users) {
        if (u.id == m_authenticatedUserId) continue; // 排除自己
        QJsonObject obj;
        obj["userId"] = u.id;
        obj["username"] = u.username;
        userArray.append(obj);
    }

    QJsonObject data;
    data["users"] = userArray;
    sendResponse(packet.requestId, MessageType::SearchUsersResponse, ErrorCode::Ok,
                 "OK", data);
}

// M3: 添加联系人
void RequestHandler::processAddContactRequest(const Packet &packet, const QJsonObject &request)
{
    const qint64 contactUserId = request.value("userId").toVariant().toLongLong();
    if (contactUserId <= 0 || contactUserId == m_authenticatedUserId) {
        sendResponse(packet.requestId, MessageType::AddContactResponse,
                     ErrorCode::CannotSendToSelf, "Invalid contact");
        return;
    }

    // 检查是否已是联系人
    auto contacts = m_db->getContacts(m_authenticatedUserId);
    for (const auto &c : contacts) {
        if (c.contactUserId == contactUserId) {
            sendResponse(packet.requestId, MessageType::AddContactResponse,
                         ErrorCode::ContactAlreadyExists, "Contact already exists");
            return;
        }
    }

    if (!m_db->addContact(m_authenticatedUserId, contactUserId)) {
        sendResponse(packet.requestId, MessageType::AddContactResponse,
                     ErrorCode::InternalError, "Failed to add contact");
        return;
    }

    // M5.5: 联系人变更写入双方同步事件流
    {
        QJsonObject ev;
        ev["contactUserId"] = contactUserId;
        m_db->appendSyncEvent(m_authenticatedUserId, "contact_added",
                              QJsonDocument(ev).toJson(QJsonDocument::Compact));
        QJsonObject evPeer;
        evPeer["contactUserId"] = m_authenticatedUserId;
        m_db->appendSyncEvent(contactUserId, "contact_added",
                              QJsonDocument(evPeer).toJson(QJsonDocument::Compact));
    }

    QJsonObject data;
    data["contactUserId"] = contactUserId;
    sendResponse(packet.requestId, MessageType::AddContactResponse, ErrorCode::Ok,
                 "Contact added", data);
}

// M3: 获取联系人列表
void RequestHandler::processGetContactsRequest(const Packet &packet)
{
    auto contacts = m_db->getContacts(m_authenticatedUserId);

    QJsonArray contactArray;
    for (const auto &c : contacts) {
        QJsonObject obj;
        obj["userId"] = c.contactUserId;
        obj["username"] = c.contactUsername;
        obj["addedAt"] = c.createdAt;
        contactArray.append(obj);
    }

    QJsonObject data;
    data["contacts"] = contactArray;
    sendResponse(packet.requestId, MessageType::GetContactsResponse, ErrorCode::Ok,
                 "OK", data);
}

// M3: 获取会话列表
void RequestHandler::processGetConversationsRequest(const Packet &packet)
{
    auto conversations = m_db->getConversationsForUser(m_authenticatedUserId);

    QJsonArray convArray;
    for (const auto &c : conversations) {
        QJsonObject obj;
        obj["conversationId"] = c.id;
        obj["type"] = c.type;
        obj["peerUserId"] = c.peerUserId;
        obj["peerUsername"] = c.peerUsername;
        obj["lastMessage"] = c.lastMessage;
        obj["lastMessageId"] = c.lastMessageId;
        obj["lastMessageAt"] = c.lastMessageAt;
        obj["unreadCount"] = c.unreadCount;
        // M9 特性栈：会话偏好（置顶/免打扰）
        obj["pinned"] = c.pinned;
        obj["muted"] = c.muted;
        // M7a: 群会话额外携带群名与成员数
        if (c.type == "group") {
            obj["name"] = c.name;
            obj["memberCount"] = c.memberCount;
        }
        convArray.append(obj);
    }

    QJsonObject data;
    data["conversations"] = convArray;
    sendResponse(packet.requestId, MessageType::GetConversationsResponse, ErrorCode::Ok,
                 "OK", data);
}

// M3: 发送消息
void RequestHandler::processSendMessageRequest(const Packet &packet, const QJsonObject &request)
{
    // M5.5: 客户端幂等键必填，重试不重复写消息
    const QString clientMessageId = request.value("clientMessageId").toString().trimmed();
    if (clientMessageId.isEmpty() || clientMessageId.size() > 128) {
        sendResponse(packet.requestId, MessageType::SendMessageResponse,
                     ErrorCode::InvalidRequest, "clientMessageId is required");
        return;
    }

    // M11: 发消息限流（连接级固定窗口，覆盖私聊与群聊两条分流路径），抑制刷消息/DoS。
    // 超限返回 RateLimited，客户端视为瞬时失败：保留 outbox 并短退避后自动重刷，不丢消息。
    if (!m_sendWindow.allow(QDateTime::currentSecsSinceEpoch())) {
        sendResponse(packet.requestId, MessageType::SendMessageResponse,
                     ErrorCode::RateLimited, "Too many messages, please slow down");
        return;
    }

    // M7a: 按目标分流：conversationId 走群聊明文路径，toUserId 走私聊 E2EE 路径
    const qint64 groupConvId = request.value("conversationId").toVariant().toLongLong();
    if (groupConvId > 0) {
        processSendGroupMessage(packet, request, groupConvId, clientMessageId);
        return;
    }

    const qint64 targetUserId = request.value("toUserId").toVariant().toLongLong();
    const QString content = request.value("content").toString();
    const QString contentType = request.value("contentType").toString("text");

    if (targetUserId <= 0 || targetUserId == m_authenticatedUserId) {
        sendResponse(packet.requestId, MessageType::SendMessageResponse,
                     ErrorCode::CannotSendToSelf, "Invalid recipient");
        return;
    }
    if (content.isEmpty()) {
        sendResponse(packet.requestId, MessageType::SendMessageResponse,
                     ErrorCode::InvalidRequest, "Message content is empty");
        return;
    }

    // M5.5 幂等重试优先：同键消息已存在时直接返回，不再校验 envelope
    // （重试时 envelope 引用的预密钥可能已被首次发送消费）
    if (auto existing = m_db->getMessageByClientKey(m_authenticatedUserId,
                                                    m_currentDeviceId, clientMessageId);
        existing.has_value()) {
        QJsonObject retryData;
        retryData["messageId"] = existing->id;
        retryData["conversationId"] = existing->conversationId;
        retryData["clientMessageId"] = clientMessageId;
        retryData["status"] = existing->status;
        retryData["reused"] = true;
        sendResponse(packet.requestId, MessageType::SendMessageResponse, ErrorCode::Ok,
                     "Message sent", retryData);
        return;
    }

    // M6: fail-closed：消息正文必须为合法 E2EE envelope（服务端只见密文）
    bool envelopeOk = false;
    const auto entries = XYChat::Security::E2eeCrypto::decodeEnvelope(content, &envelopeOk);
    if (!envelopeOk) {
        sendResponse(packet.requestId, MessageType::SendMessageResponse,
                     ErrorCode::E2eeInvalidEnvelope,
                     "Message content must be a valid E2EE envelope");
        return;
    }

    // 逐条校验：无重复设备（自身拷贝与接收方条目分开去重，同一台机器上
    // 收发双方 deviceId 可能相同）；接收方条目引用的预密钥必须处于 claimed
    // 状态；发送方自身设备的拷贝条目（prekeyId=0，仅身份密钥加密）无需预密钥
    QList<qint64> prekeyIds;
    QSet<QString> seenPeerDevices;
    bool seenSelfCopy = false;
    for (const auto &entry : entries) {
        const bool isSelfCopy = entry.deviceId == m_currentDeviceId
            && entry.prekeyId == XYChat::Security::E2eeCrypto::SelfCopyPrekeyId;
        if (isSelfCopy) {
            if (seenSelfCopy) {
                sendResponse(packet.requestId, MessageType::SendMessageResponse,
                             ErrorCode::E2eeInvalidEnvelope, "Duplicate device entry in envelope");
                return;
            }
            seenSelfCopy = true;
            continue; // 发送方自己设备的密文拷贝，内容对服务端仍不可见
        }
        if (seenPeerDevices.contains(entry.deviceId)) {
            sendResponse(packet.requestId, MessageType::SendMessageResponse,
                         ErrorCode::E2eeInvalidEnvelope, "Duplicate device entry in envelope");
            return;
        }
        seenPeerDevices.insert(entry.deviceId);
        if (!m_db->validateClaimedPrekey(targetUserId, entry.deviceId, entry.prekeyId)) {
            sendResponse(packet.requestId, MessageType::SendMessageResponse,
                         ErrorCode::E2eeInvalidEnvelope,
                         "Envelope references invalid or unconsumable prekey");
            return;
        }
        prekeyIds.append(entry.prekeyId);
    }

    // 至少需要一个接收方设备条目（纯自身拷贝不构成消息投递）
    if (prekeyIds.isEmpty()) {
        sendResponse(packet.requestId, MessageType::SendMessageResponse,
                     ErrorCode::E2eeInvalidEnvelope,
                     "Envelope has no recipient device entry");
        return;
    }

    // 获取或创建会话
    const qint64 convId = m_db->getOrCreatePrivateConversation(m_authenticatedUserId, targetUserId);
    if (convId < 0) {
        sendResponse(packet.requestId, MessageType::SendMessageResponse,
                     ErrorCode::InternalError, "Failed to create conversation");
        return;
    }

    // M6 审查修复：消息入库与预密钥消费绑定为原子单元，
    // 避免消息已投递但预密钥悬挂在 claimed 状态
    m_db->beginTransaction();

    // 存储消息（幂等：并发重复提交仍由幂等键兜底）
    const qint64 msgId = m_db->sendMessage(convId, m_authenticatedUserId, content,
                                           contentType, clientMessageId, m_currentDeviceId);
    if (msgId < 0) {
        m_db->rollbackTransaction();
        sendResponse(packet.requestId, MessageType::SendMessageResponse,
                     ErrorCode::InternalError, "Failed to send message");
        return;
    }

    // 消息入库后消费预密钥（claimed -> used），保证一次性投递
    m_db->consumePrekeys(prekeyIds);

    if (!m_db->commitTransaction()) {
        m_db->rollbackTransaction();
        sendResponse(packet.requestId, MessageType::SendMessageResponse,
                     ErrorCode::InternalError, "Failed to send message");
        return;
    }

    const QString createdAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    // M5.5: 写入接收方同步事件流（发送方其他设备同样可同步）
    {
        QJsonObject ev;
        ev["messageId"] = msgId;
        ev["conversationId"] = convId;
        ev["senderId"] = m_authenticatedUserId;
        ev["content"] = content;
        ev["contentType"] = contentType;
        ev["clientMessageId"] = clientMessageId;
        ev["createdAt"] = createdAt;
        const QString evJson = QJsonDocument(ev).toJson(QJsonDocument::Compact);
        m_db->appendSyncEvent(targetUserId, "message", evJson);
        m_db->appendSyncEvent(m_authenticatedUserId, "message", evJson);
    }

    // 构造消息通知包，用于转发给在线接收方
    QJsonObject notifyJson;
    notifyJson["messageId"] = msgId;
    notifyJson["conversationId"] = convId;
    notifyJson["senderId"] = m_authenticatedUserId;
    notifyJson["content"] = content;
    notifyJson["contentType"] = contentType;
    notifyJson["createdAt"] = createdAt;

    Packet notifyPacket;
    notifyPacket.messageType = MessageType::NewMessageNotification;
    notifyPacket.requestId = 0;
    notifyPacket.payload = QJsonDocument(notifyJson).toJson(QJsonDocument::Compact);

    // 通过信号通知 Server 转发
    emit messageForUser(targetUserId, PacketCodec::encode(notifyPacket));

    // 返回发送确认给发送方（回传幂等键便于客户端匹配）
    QJsonObject data;
    data["messageId"] = msgId;
    data["conversationId"] = convId;
    data["clientMessageId"] = clientMessageId;
    data["status"] = "sent";
    sendResponse(packet.requestId, MessageType::SendMessageResponse, ErrorCode::Ok,
                 "Message sent", data);
}

// M3: 确认消息
void RequestHandler::processAckMessageRequest(const Packet &packet, const QJsonObject &request)
{
    const qint64 messageId = request.value("messageId").toVariant().toLongLong();
    const QString status = request.value("status").toString("delivered");

    if (messageId <= 0) {
        sendResponse(packet.requestId, MessageType::AckMessageResponse,
                     ErrorCode::InvalidRequest, "Invalid messageId");
        return;
    }
    if (status != "delivered" && status != "read") {
        sendResponse(packet.requestId, MessageType::AckMessageResponse,
                     ErrorCode::InvalidRequest, "Status must be 'delivered' or 'read'");
        return;
    }

    auto msgOpt = m_db->getMessage(messageId);
    if (!msgOpt.has_value()) {
        sendResponse(packet.requestId, MessageType::AckMessageResponse,
                     ErrorCode::MessageNotFound, "Message not found");
        return;
    }

    // M5.5: 先授权再更新：请求者必须是消息所属会话的成员
    if (!m_db->isConversationMember(msgOpt->conversationId, m_authenticatedUserId)) {
        sendResponse(packet.requestId, MessageType::AckMessageResponse,
                     ErrorCode::PermissionDenied, "Not a member of this conversation");
        return;
    }

    // M5.5: 按接收者/设备维度记录回执，替代全局单值状态
    m_db->recordMessageReceipt(messageId, m_authenticatedUserId, m_currentDeviceId, status);

    if (status == "read") {
        m_db->updateMemberReadCursor(msgOpt->conversationId, m_authenticatedUserId, messageId);

        // M9: 已读状态多端同步——向已读者自身事件流追加 read_cursor 并实时推送给其
        // 所有在线设备（含本机，幂等）；其他设备据此清零未读角标、把该会话中
        // messageId 及之前的对方消息标记为已读
        QJsonObject readEv;
        readEv["conversationId"] = msgOpt->conversationId;
        readEv["readMessageId"] = messageId;
        const QByteArray readPayload = QJsonDocument(readEv).toJson(QJsonDocument::Compact);
        m_db->appendSyncEvent(m_authenticatedUserId, "read_cursor", QString::fromUtf8(readPayload));

        Packet readPacket;
        readPacket.messageType = MessageType::ReadCursorNotification;
        readPacket.requestId = 0;
        readPacket.payload = readPayload;
        emit messageForUser(m_authenticatedUserId, PacketCodec::encode(readPacket));
    }

    // M7a: 按接收者总数聚合展示状态（私聊接收者为 1 人，群聊为除发送方外全体成员），
    // 修复此前“任一回执即聚合”在群聊下提前达成 read/delivered 的问题
    const int expectedRecipients =
        m_db->memberCountExcluding(msgOpt->conversationId, msgOpt->senderId);
    // M7a: 按接收用户去重计数，避免单用户多设备回执导致提前达成
    const int readCount = m_db->receiptUserCount(messageId, "read");
    const int deliveredCount = m_db->receiptUserCount(messageId, "delivered");

    QString aggregatedStatus;
    if (expectedRecipients > 0 && readCount >= expectedRecipients) {
        aggregatedStatus = "read";
    } else if (expectedRecipients > 0 && deliveredCount >= expectedRecipients) {
        aggregatedStatus = "delivered";
    }
    if (!aggregatedStatus.isEmpty() && aggregatedStatus != msgOpt->status) {
        m_db->updateMessageStatus(messageId, aggregatedStatus);

        // 推送状态更新给发送方（多设备可经 sync_events 同步）；
        // 群消息额外携带送达/已读计数
        QJsonObject statusJson;
        statusJson["messageId"] = messageId;
        statusJson["conversationId"] = msgOpt->conversationId;
        statusJson["status"] = aggregatedStatus;
        statusJson["deliveredCount"] = deliveredCount;
        statusJson["readCount"] = readCount;
        Packet statusPacket;
        statusPacket.messageType = MessageType::MessageStatusUpdate;
        statusPacket.requestId = 0;
        statusPacket.payload = QJsonDocument(statusJson).toJson(QJsonDocument::Compact);
        emit messageForUser(msgOpt->senderId, PacketCodec::encode(statusPacket));

        // 回执写入发送方同步事件流
        QJsonObject ev;
        ev["messageId"] = messageId;
        ev["conversationId"] = msgOpt->conversationId;
        ev["status"] = aggregatedStatus;
        ev["byUserId"] = m_authenticatedUserId;
        ev["deliveredCount"] = deliveredCount;
        ev["readCount"] = readCount;
        m_db->appendSyncEvent(msgOpt->senderId, "receipt",
                              QJsonDocument(ev).toJson(QJsonDocument::Compact));
    }

    QJsonObject data;
    data["messageId"] = messageId;
    data["status"] = status;
    sendResponse(packet.requestId, MessageType::AckMessageResponse, ErrorCode::Ok,
                 "OK", data);
}

// M3: 同步消息
void RequestHandler::processSyncMessagesRequest(const Packet &packet, const QJsonObject &request)
{
    const qint64 conversationId = request.value("conversationId").toVariant().toLongLong();
    const qint64 afterId = request.value("afterId").toVariant().toLongLong();
    const int limit = request.value("limit").toInt(100);

    if (conversationId <= 0) {
        sendResponse(packet.requestId, MessageType::SyncMessagesResponse,
                     ErrorCode::InvalidRequest, "Invalid conversationId");
        return;
    }

    // M5.5: 先授权再查询：验证用户是该会话的成员
    if (!m_db->isConversationMember(conversationId, m_authenticatedUserId)) {
        sendResponse(packet.requestId, MessageType::SyncMessagesResponse,
                     ErrorCode::PermissionDenied, "Not a member of this conversation");
        return;
    }

    auto messages = m_db->syncMessages(conversationId, afterId, limit);

    QJsonArray msgArray;
    for (const auto &m : messages) {
        QJsonObject obj;
        obj["messageId"] = m.id;
        obj["conversationId"] = m.conversationId;
        obj["senderId"] = m.senderId;
        obj["senderUsername"] = m.senderUsername;
        obj["content"] = m.content;
        obj["contentType"] = m.contentType;
        obj["status"] = m.status;
        obj["createdAt"] = m.createdAt;
        // M9 特性栈：编辑/删除标记（离线重登经 sync_messages 重建编辑/删除状态）
        obj["deleted"] = m.deleted;
        if (!m.editedAt.isEmpty()) {
            obj["edited"] = true;
            obj["editedAt"] = m.editedAt;
        }
        msgArray.append(obj);
    }

    // M5.5: 拉取同步时仅前进读游标，不再修改全局消息状态；
    // 已读回执由客户端显式 ack_message 产生
    if (!messages.isEmpty()) {
        m_db->updateMemberReadCursor(conversationId, m_authenticatedUserId,
                                     messages.last().id);
    }

    QJsonObject data;
    data["conversationId"] = conversationId;
    data["messages"] = msgArray;
    data["hasMore"] = (messages.size() >= limit);
    sendResponse(packet.requestId, MessageType::SyncMessagesResponse, ErrorCode::Ok,
                 "OK", data);
}

// M5.5: 账号级增量同步
void RequestHandler::processSyncEventsRequest(const Packet &packet, const QJsonObject &request)
{
    const qint64 afterSeq = request.value("afterSeq").toVariant().toLongLong();
    int limit = request.value("limit").toInt(200);
    if (afterSeq < 0) {
        sendResponse(packet.requestId, MessageType::SyncEventsResponse,
                     ErrorCode::InvalidRequest, "Invalid afterSeq");
        return;
    }
    if (limit <= 0 || limit > 1000) {
        limit = 200;
    }

    // M9: 落后于清理水位的设备需全量回退——afterSeq 与水位线之间的事件已被清理，
    // 增量拉取会静默丢失这段历史，故返回 needsFullSync 让客户端重拉会话/消息全量
    const qint64 prunedBelow = m_db->prunedBelowSeq();
    if (afterSeq > 0 && afterSeq < prunedBelow) {
        QJsonObject data;
        data["events"] = QJsonArray();
        data["lastSeq"] = afterSeq;
        data["hasMore"] = false;
        data["needsFullSync"] = true;
        // 事件表被清空时 maxSyncEventSeq 为 0，回退到水位线（已删事件最大 seq，
        // AUTOINCREMENT 不复用）保证客户端游标能前进并自愈，避免反复触发全量回退
        data["fullSyncSeq"] = qMax(m_db->maxSyncEventSeq(), prunedBelow);
        sendResponse(packet.requestId, MessageType::SyncEventsResponse, ErrorCode::Ok,
                     "OK", data);
        return;
    }

    // 仅返回本人事件流，无越权面
    const auto events = m_db->getSyncEvents(m_authenticatedUserId, afterSeq, limit);

    QJsonArray eventArray;
    for (const auto &ev : events) {
        QJsonObject obj;
        obj["seq"] = ev.seq;
        obj["type"] = ev.eventType;
        obj["payload"] = QJsonDocument::fromJson(ev.payload.toUtf8()).object();
        obj["createdAt"] = ev.createdAt;
        eventArray.append(obj);
    }

    QJsonObject data;
    data["events"] = eventArray;
    data["lastSeq"] = events.isEmpty() ? afterSeq : events.last().seq;
    data["hasMore"] = (events.size() >= limit);
    data["needsFullSync"] = false;
    sendResponse(packet.requestId, MessageType::SyncEventsResponse, ErrorCode::Ok,
                 "OK", data);
}

// M6: 密钥注册（身份公钥 + 一次性预密钥公钥）
void RequestHandler::processRegisterKeysRequest(const Packet &packet, const QJsonObject &request)
{
    // 仅允许注册当前认证设备自己的密钥（deviceId 取自 session，不信任请求参数）
    const QString identityPub = request.value("identityPub").toString().trimmed();
    if (identityPub.isEmpty()
        || QByteArray::fromBase64(identityPub.toLatin1(),
                                  QByteArray::AbortOnBase64DecodingErrors).size() != 32) {
        sendResponse(packet.requestId, MessageType::RegisterKeysResponse,
                     ErrorCode::InvalidRequest,
                     "identityPub must be a 32-byte Base64 X25519 public key");
        return;
    }

    if (!m_db->upsertIdentityKey(m_authenticatedUserId, m_currentDeviceId, identityPub)) {
        sendResponse(packet.requestId, MessageType::RegisterKeysResponse,
                     ErrorCode::InternalError, "Failed to store identity key");
        return;
    }

    // 可选：批量上传一次性预密钥公钥
    int uploaded = 0;
    const QJsonArray prekeyArray = request.value("prekeys").toArray();
    if (!prekeyArray.isEmpty()) {
        if (prekeyArray.size() > MaxPrekeysPerBatch) {
            sendResponse(packet.requestId, MessageType::RegisterKeysResponse,
                         ErrorCode::InvalidRequest,
                         QString("At most %1 prekeys per batch").arg(MaxPrekeysPerBatch));
            return;
        }
        const int remaining = m_db->prekeyCount(m_authenticatedUserId, m_currentDeviceId);
        if (remaining + prekeyArray.size() > MaxPrekeysPerDevice) {
            sendResponse(packet.requestId, MessageType::RegisterKeysResponse,
                         ErrorCode::InvalidRequest,
                         QString("Prekey quota exceeded (%1 unused allowed)")
                             .arg(MaxPrekeysPerDevice));
            return;
        }

        QStringList pubs;
        bool formatOk = true;
        for (const QJsonValue &value : prekeyArray) {
            const QString pub = value.toString().trimmed();
            if (QByteArray::fromBase64(pub.toLatin1(),
                                       QByteArray::AbortOnBase64DecodingErrors).size() != 32) {
                formatOk = false;
                break;
            }
            pubs.append(pub);
        }
        if (!formatOk) {
            sendResponse(packet.requestId, MessageType::RegisterKeysResponse,
                         ErrorCode::InvalidRequest,
                         "Each prekey must be a 32-byte Base64 X25519 public key");
            return;
        }

        uploaded = m_db->uploadPrekeys(m_authenticatedUserId, m_currentDeviceId, pubs);
        if (uploaded < 0) {
            sendResponse(packet.requestId, MessageType::RegisterKeysResponse,
                         ErrorCode::InternalError, "Failed to store prekeys");
            return;
        }
    }

    QJsonObject data;
    data["deviceId"] = m_currentDeviceId;
    data["uploadedPrekeys"] = uploaded;
    data["remainingPrekeys"] = m_db->prekeyCount(m_authenticatedUserId, m_currentDeviceId);
    sendResponse(packet.requestId, MessageType::RegisterKeysResponse, ErrorCode::Ok,
                 "Keys registered", data);
    qDebug() << "[Handler] User" << m_authenticatedUserId << "registered E2EE keys,"
             << uploaded << "prekeys uploaded";
}

// M6: 拉取目标用户密钥包（每设备身份公钥 + 一个认领的预密钥）
void RequestHandler::processFetchKeysRequest(const Packet &packet, const QJsonObject &request)
{
    const qint64 targetUserId = request.value("userId").toVariant().toLongLong();
    if (targetUserId <= 0 || targetUserId == m_authenticatedUserId) {
        sendResponse(packet.requestId, MessageType::FetchKeysResponse,
                     ErrorCode::CannotSendToSelf, "Invalid target user");
        return;
    }

    // M6 审查修复：连接级频率限制，防止恶意循环拉取耗尽他人预密钥池
    // （M11：改用 RateWindow，超限返回 RateLimited，由结构化审计日志记录）
    if (!m_fetchKeysWindow.allow(QDateTime::currentSecsSinceEpoch())) {
        sendResponse(packet.requestId, MessageType::FetchKeysResponse,
                     ErrorCode::RateLimited,
                     "Too many key bundle requests, please slow down");
        return;
    }

    // 目标用户必须已注册过设备（避免对任意 userId 做密钥探测）
    if (m_db->getDevicesByUserId(targetUserId).isEmpty()) {
        sendResponse(packet.requestId, MessageType::FetchKeysResponse,
                     ErrorCode::AccountNotFound, "Target user has no registered devices");
        return;
    }

    // 事务内为每个有库存的设备认领一个预密钥
    const auto claimed = m_db->claimPrekeys(targetUserId);
    if (claimed.isEmpty()) {
        sendResponse(packet.requestId, MessageType::FetchKeysResponse,
                     ErrorCode::KeyBundleUnavailable,
                     "Target user has no available prekeys");
        return;
    }

    // 组装密钥包：跳过未注册身份公钥的设备（无法加密）
    QHash<QString, QString> identityByDevice;
    for (const auto &key : m_db->getIdentityKeysByUser(targetUserId)) {
        identityByDevice.insert(key.deviceId, key.identityPub);
    }

    QJsonArray bundles;
    for (const auto &c : claimed) {
        const QString identityPub = identityByDevice.value(c.deviceId);
        if (identityPub.isEmpty()) {
            qDebug() << "[Handler] FetchKeys: device" << c.deviceId
                     << "of user" << targetUserId << "has no identity key, skipped";
            continue;
        }
        QJsonObject obj;
        obj["deviceId"] = c.deviceId;
        obj["identityPub"] = identityPub;
        obj["prekeyId"] = c.prekeyId;
        obj["prekeyPub"] = c.prekeyPub;
        bundles.append(obj);
    }

    if (bundles.isEmpty()) {
        sendResponse(packet.requestId, MessageType::FetchKeysResponse,
                     ErrorCode::KeyBundleUnavailable,
                     "Target user devices have no identity keys");
        return;
    }

    QJsonObject data;
    data["userId"] = targetUserId;
    data["bundles"] = bundles;
    sendResponse(packet.requestId, MessageType::FetchKeysResponse, ErrorCode::Ok,
                 "OK", data);
}

// M7a: 群系统消息入库并 fan-out（contentType=system，无幂等键，不推送发送操作者）
void RequestHandler::postGroupSystemMessage(qint64 conversationId, qint64 operatorId,
                                            const QJsonObject &payload)
{
    const QString content = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    const qint64 msgId = m_db->sendMessage(conversationId, operatorId, content, "system");
    if (msgId < 0) {
        StructuredLogger::event(LogLevel::Warning, "group.system_message_failed")
            .userId(operatorId).field("conversationId", conversationId).write();
        return;
    }

    const QString createdAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    const QString senderUsername = m_db->usernameById(operatorId);

    QJsonObject ev;
    ev["messageId"] = msgId;
    ev["conversationId"] = conversationId;
    ev["senderId"] = operatorId;
    ev["content"] = content;
    ev["contentType"] = "system";
    ev["createdAt"] = createdAt;
    const QString evJson = QJsonDocument(ev).toJson(QJsonDocument::Compact);

    QJsonObject notifyJson = ev;
    notifyJson["senderUsername"] = senderUsername;
    Packet notifyPacket;
    notifyPacket.messageType = MessageType::NewMessageNotification;
    notifyPacket.requestId = 0;
    notifyPacket.payload = QJsonDocument(notifyJson).toJson(QJsonDocument::Compact);
    const QByteArray encoded = PacketCodec::encode(notifyPacket);

    // 系统消息面向全体成员：事件流全员写入（含操作者其他设备），
    // 实时推送仅面向除操作者外的成员
    for (qint64 memberId : m_db->getGroupMemberIds(conversationId)) {
        m_db->appendSyncEvent(memberId, "message", evJson);
        if (memberId != operatorId) {
            emit messageForUser(memberId, encoded);
        }
    }
}

// M7a: 群变更通知：推送 GroupChangedNotification 并写入全体现任成员 sync_events
void RequestHandler::notifyGroupChanged(qint64 conversationId, const QString &changeType,
                                        qint64 operatorId, qint64 targetUserId)
{
    const int memberCount = m_db->getGroupMemberIds(conversationId).size();

    QJsonObject payload;
    payload["conversationId"] = conversationId;
    payload["changeType"] = changeType;
    payload["operatorId"] = operatorId;
    if (targetUserId > 0) {
        payload["targetUserId"] = targetUserId;
    }
    payload["memberCount"] = memberCount;
    const QByteArray payloadJson = QJsonDocument(payload).toJson(QJsonDocument::Compact);

    Packet notifyPacket;
    notifyPacket.messageType = MessageType::GroupChangedNotification;
    notifyPacket.requestId = 0;
    notifyPacket.payload = payloadJson;
    const QByteArray encoded = PacketCodec::encode(notifyPacket);

    for (qint64 memberId : m_db->getGroupMemberIds(conversationId)) {
        m_db->appendSyncEvent(memberId, "group_changed", QString::fromUtf8(payloadJson));
        emit messageForUser(memberId, encoded);
    }
}

// M7a: 创建群组
void RequestHandler::processCreateGroupRequest(const Packet &packet, const QJsonObject &request)
{
    const QString name = request.value("name").toString().trimmed();
    if (name.isEmpty() || name.size() > MaxGroupNameLength) {
        sendResponse(packet.requestId, MessageType::CreateGroupResponse,
                     ErrorCode::InvalidRequest,
                     QString("Group name must be 1-%1 characters").arg(MaxGroupNameLength));
        return;
    }

    // 解析并清洗初始成员：去重、剔除创建者自身、拒绝非法 ID
    const QJsonArray memberArray = request.value("memberIds").toArray();
    if (memberArray.size() > DatabaseManager::MaxInviteBatch) {
        sendResponse(packet.requestId, MessageType::CreateGroupResponse,
                     ErrorCode::GroupLimitExceeded,
                     QString("At most %1 members per invite").arg(DatabaseManager::MaxInviteBatch));
        return;
    }

    QList<qint64> memberIds;
    QSet<qint64> seen;
    for (const QJsonValue &v : memberArray) {
        const qint64 id = v.toVariant().toLongLong();
        if (id <= 0 || id == m_authenticatedUserId || seen.contains(id)) {
            continue;
        }
        seen.insert(id);
        memberIds.append(id);
    }

    // 含创建者的总人数不得超限
    if (memberIds.size() + 1 > DatabaseManager::MaxGroupMembers) {
        sendResponse(packet.requestId, MessageType::CreateGroupResponse,
                     ErrorCode::GroupLimitExceeded,
                     QString("Group members limited to %1").arg(DatabaseManager::MaxGroupMembers));
        return;
    }

    // 初始成员必须均为已注册用户
    for (qint64 id : memberIds) {
        if (m_db->usernameById(id).isEmpty()) {
            sendResponse(packet.requestId, MessageType::CreateGroupResponse,
                         ErrorCode::AccountNotFound,
                         QString("User %1 does not exist").arg(id));
            return;
        }
    }

    const qint64 convId = m_db->createGroup(m_authenticatedUserId, name, memberIds);
    if (convId < 0) {
        sendResponse(packet.requestId, MessageType::CreateGroupResponse,
                     ErrorCode::InternalError, "Failed to create group");
        return;
    }

    qDebug() << "[Handler] User" << m_authenticatedUserId << "created group" << convId
             << "with" << memberIds.size() << "initial members";

    // 建群系统消息（面向初始成员）与群变更通知
    QJsonObject sysPayload;
    sysPayload["event"] = "group_created";
    sysPayload["operatorId"] = m_authenticatedUserId;
    sysPayload["name"] = name;
    postGroupSystemMessage(convId, m_authenticatedUserId, sysPayload);
    notifyGroupChanged(convId, "group_created", m_authenticatedUserId, 0);

    QJsonObject data;
    data["conversationId"] = convId;
    data["name"] = name;
    data["memberCount"] = m_db->getGroupMemberIds(convId).size();
    sendResponse(packet.requestId, MessageType::CreateGroupResponse, ErrorCode::Ok,
                 "Group created", data);
}

// M7a: 邀请成员入群
void RequestHandler::processInviteGroupMembersRequest(const Packet &packet, const QJsonObject &request)
{
    const qint64 convId = request.value("conversationId").toVariant().toLongLong();
    if (convId <= 0) {
        sendResponse(packet.requestId, MessageType::InviteGroupMembersResponse,
                     ErrorCode::InvalidRequest, "Invalid conversationId");
        return;
    }
    auto conv = m_db->getConversation(convId);
    if (!conv.has_value() || conv->type != "group") {
        sendResponse(packet.requestId, MessageType::InviteGroupMembersResponse,
                     ErrorCode::ConversationNotFound, "Group not found");
        return;
    }

    // 先授权：仅群成员可邀请
    if (!m_db->isConversationMember(convId, m_authenticatedUserId)) {
        sendResponse(packet.requestId, MessageType::InviteGroupMembersResponse,
                     ErrorCode::PermissionDenied, "Not a member of this group");
        return;
    }

    const QJsonArray userArray = request.value("userIds").toArray();
    if (userArray.isEmpty()) {
        sendResponse(packet.requestId, MessageType::InviteGroupMembersResponse,
                     ErrorCode::InvalidRequest, "userIds is required");
        return;
    }
    if (userArray.size() > DatabaseManager::MaxInviteBatch) {
        sendResponse(packet.requestId, MessageType::InviteGroupMembersResponse,
                     ErrorCode::GroupLimitExceeded,
                     QString("At most %1 members per invite").arg(DatabaseManager::MaxInviteBatch));
        return;
    }

    // 逐个校验：合法、已注册、未在群中；受成员上限约束
    QList<qint64> toAdd;
    QSet<qint64> seen;
    int currentCount = m_db->getGroupMemberIds(convId).size();
    for (const QJsonValue &v : userArray) {
        const qint64 id = v.toVariant().toLongLong();
        if (id <= 0 || id == m_authenticatedUserId || seen.contains(id)) {
            continue;
        }
        seen.insert(id);
        if (m_db->usernameById(id).isEmpty()) {
            sendResponse(packet.requestId, MessageType::InviteGroupMembersResponse,
                         ErrorCode::AccountNotFound,
                         QString("User %1 does not exist").arg(id));
            return;
        }
        if (m_db->isConversationMember(convId, id)) {
            sendResponse(packet.requestId, MessageType::InviteGroupMembersResponse,
                         ErrorCode::MemberAlreadyExists,
                         QString("User %1 is already in the group").arg(id));
            return;
        }
        if (currentCount + toAdd.size() + 1 > DatabaseManager::MaxGroupMembers) {
            sendResponse(packet.requestId, MessageType::InviteGroupMembersResponse,
                         ErrorCode::GroupLimitExceeded,
                         QString("Group members limited to %1").arg(DatabaseManager::MaxGroupMembers));
            return;
        }
        toAdd.append(id);
    }
    if (toAdd.isEmpty()) {
        sendResponse(packet.requestId, MessageType::InviteGroupMembersResponse,
                     ErrorCode::InvalidRequest, "No valid users to invite");
        return;
    }

    if (!m_db->addGroupMembers(convId, toAdd)) {
        sendResponse(packet.requestId, MessageType::InviteGroupMembersResponse,
                     ErrorCode::InternalError, "Failed to add members");
        return;
    }

    qDebug() << "[Handler] User" << m_authenticatedUserId << "invited" << toAdd.size()
             << "members to group" << convId;

    // 系统消息 + 群变更通知（面向变更后的全体成员，含新成员）
    QJsonObject sysPayload;
    sysPayload["event"] = "member_added";
    sysPayload["operatorId"] = m_authenticatedUserId;
    QJsonArray addedArray;
    for (qint64 id : toAdd) {
        addedArray.append(id);
    }
    sysPayload["targetUserIds"] = addedArray;
    postGroupSystemMessage(convId, m_authenticatedUserId, sysPayload);
    notifyGroupChanged(convId, "member_added", m_authenticatedUserId, toAdd.first());

    QJsonObject data;
    data["conversationId"] = convId;
    data["added"] = addedArray;
    data["memberCount"] = m_db->getGroupMemberIds(convId).size();
    sendResponse(packet.requestId, MessageType::InviteGroupMembersResponse, ErrorCode::Ok,
                 "Members invited", data);
}

// M7a: 退出群组（群主退群自动转让群主给最早入群成员，避免无主群）
void RequestHandler::processLeaveGroupRequest(const Packet &packet, const QJsonObject &request)
{
    const qint64 convId = request.value("conversationId").toVariant().toLongLong();
    if (convId <= 0) {
        sendResponse(packet.requestId, MessageType::LeaveGroupResponse,
                     ErrorCode::InvalidRequest, "Invalid conversationId");
        return;
    }
    auto conv = m_db->getConversation(convId);
    if (!conv.has_value() || conv->type != "group") {
        sendResponse(packet.requestId, MessageType::LeaveGroupResponse,
                     ErrorCode::ConversationNotFound, "Group not found");
        return;
    }

    const QString role = m_db->groupRole(convId, m_authenticatedUserId);
    if (role.isEmpty()) {
        sendResponse(packet.requestId, MessageType::LeaveGroupResponse,
                     ErrorCode::MemberNotFound, "Not a member of this group");
        return;
    }

    // 群主退群：若仍有其他成员，先自动转让群主（降级策略）
    if (role == "owner") {
        qint64 successor = 0;
        for (const auto &member : m_db->getGroupMembers(convId)) {
            const qint64 memberId = member["userId"].toVariant().toLongLong();
            if (memberId != m_authenticatedUserId) {
                successor = memberId;
                break;
            }
        }
        if (successor > 0) {
            m_db->updateMemberRole(convId, successor, "owner");
            QJsonObject transferPayload;
            transferPayload["event"] = "owner_transferred";
            transferPayload["operatorId"] = m_authenticatedUserId;
            transferPayload["newOwnerId"] = successor;
            postGroupSystemMessage(convId, m_authenticatedUserId, transferPayload);
            notifyGroupChanged(convId, "owner_transferred", m_authenticatedUserId, successor);
        }
    }

    if (!m_db->removeGroupMember(convId, m_authenticatedUserId)) {
        sendResponse(packet.requestId, MessageType::LeaveGroupResponse,
                     ErrorCode::InternalError, "Failed to leave group");
        return;
    }

    qDebug() << "[Handler] User" << m_authenticatedUserId << "left group" << convId;

    // 系统消息与变更通知面向剩余成员（退出者本人不再接收）
    QJsonObject sysPayload;
    sysPayload["event"] = "member_left";
    sysPayload["operatorId"] = m_authenticatedUserId;
    postGroupSystemMessage(convId, m_authenticatedUserId, sysPayload);
    notifyGroupChanged(convId, "member_left", m_authenticatedUserId, m_authenticatedUserId);

    QJsonObject data;
    data["conversationId"] = convId;
    sendResponse(packet.requestId, MessageType::LeaveGroupResponse, ErrorCode::Ok,
                 "Left group", data);
}

// M7a: 移除群成员（仅群主/管理员，层级保护：不可移除同级或更高层级）
void RequestHandler::processKickGroupMemberRequest(const Packet &packet, const QJsonObject &request)
{
    const qint64 convId = request.value("conversationId").toVariant().toLongLong();
    const qint64 targetUserId = request.value("userId").toVariant().toLongLong();
    if (convId <= 0 || targetUserId <= 0) {
        sendResponse(packet.requestId, MessageType::KickGroupMemberResponse,
                     ErrorCode::InvalidRequest, "Invalid conversationId or userId");
        return;
    }
    auto conv = m_db->getConversation(convId);
    if (!conv.has_value() || conv->type != "group") {
        sendResponse(packet.requestId, MessageType::KickGroupMemberResponse,
                     ErrorCode::ConversationNotFound, "Group not found");
        return;
    }
    if (targetUserId == m_authenticatedUserId) {
        sendResponse(packet.requestId, MessageType::KickGroupMemberResponse,
                     ErrorCode::InvalidRequest, "Cannot kick yourself, use leave_group");
        return;
    }

    const QString myRole = m_db->groupRole(convId, m_authenticatedUserId);
    const QString targetRole = m_db->groupRole(convId, targetUserId);
    if (targetRole.isEmpty()) {
        sendResponse(packet.requestId, MessageType::KickGroupMemberResponse,
                     ErrorCode::MemberNotFound, "Target is not a member of this group");
        return;
    }
    // 层级保护：owner 可移除 admin/member；admin 仅可移除 member；member 无权
    const bool allowed = (myRole == "owner" && targetRole != "owner")
        || (myRole == "admin" && targetRole == "member");
    if (!allowed) {
        sendResponse(packet.requestId, MessageType::KickGroupMemberResponse,
                     ErrorCode::NotGroupOwner, "Insufficient role to remove this member");
        return;
    }

    if (!m_db->removeGroupMember(convId, targetUserId)) {
        sendResponse(packet.requestId, MessageType::KickGroupMemberResponse,
                     ErrorCode::InternalError, "Failed to remove member");
        return;
    }

    qDebug() << "[Handler] User" << m_authenticatedUserId << "kicked" << targetUserId
             << "from group" << convId;

    // 系统消息与变更通知面向剩余成员（被移除者不再接收；其本地会话由客户端清理）
    QJsonObject sysPayload;
    sysPayload["event"] = "member_removed";
    sysPayload["operatorId"] = m_authenticatedUserId;
    sysPayload["targetUserId"] = targetUserId;
    postGroupSystemMessage(convId, m_authenticatedUserId, sysPayload);
    notifyGroupChanged(convId, "member_removed", m_authenticatedUserId, targetUserId);

    QJsonObject data;
    data["conversationId"] = convId;
    data["removedUserId"] = targetUserId;
    sendResponse(packet.requestId, MessageType::KickGroupMemberResponse, ErrorCode::Ok,
                 "Member removed", data);
}

// M7a: 获取群信息（仅群成员）
void RequestHandler::processGetGroupInfoRequest(const Packet &packet, const QJsonObject &request)
{
    const qint64 convId = request.value("conversationId").toVariant().toLongLong();
    if (convId <= 0) {
        sendResponse(packet.requestId, MessageType::GetGroupInfoResponse,
                     ErrorCode::InvalidRequest, "Invalid conversationId");
        return;
    }
    auto conv = m_db->getConversation(convId);
    if (!conv.has_value() || conv->type != "group") {
        sendResponse(packet.requestId, MessageType::GetGroupInfoResponse,
                     ErrorCode::ConversationNotFound, "Group not found");
        return;
    }

    // 先授权再查询
    const QString myRole = m_db->groupRole(convId, m_authenticatedUserId);
    if (myRole.isEmpty()) {
        sendResponse(packet.requestId, MessageType::GetGroupInfoResponse,
                     ErrorCode::PermissionDenied, "Not a member of this group");
        return;
    }

    QJsonArray memberArray;
    for (const auto &member : m_db->getGroupMembers(convId)) {
        memberArray.append(member);
    }

    QJsonObject data;
    data["conversationId"] = convId;
    data["name"] = conv->name;
    data["memberCount"] = memberArray.size();
    data["myRole"] = myRole;
    data["members"] = memberArray;
    sendResponse(packet.requestId, MessageType::GetGroupInfoResponse, ErrorCode::Ok,
                 "OK", data);
}

// M7b: 拉取群内所有成员（含发送方自己）的 E2EE 密钥包，
// 供客户端通过 pairwise E2EE 分发 Sender Key
void RequestHandler::processFetchGroupKeysRequest(const Packet &packet, const QJsonObject &request)
{
    const qint64 convId = request.value("conversationId").toVariant().toLongLong();
    if (convId <= 0) {
        sendResponse(packet.requestId, MessageType::FetchGroupKeysResponse,
                     ErrorCode::InvalidRequest, "Invalid conversationId");
        return;
    }

    auto conv = m_db->getConversation(convId);
    if (!conv.has_value() || conv->type != "group") {
        sendResponse(packet.requestId, MessageType::FetchGroupKeysResponse,
                     ErrorCode::ConversationNotFound, "Group not found");
        return;
    }

    // 先授权：仅群成员可拉取群内密钥包
    if (!m_db->isConversationMember(convId, m_authenticatedUserId)) {
        sendResponse(packet.requestId, MessageType::FetchGroupKeysResponse,
                     ErrorCode::PermissionDenied, "Not a member of this group");
        return;
    }

    // M6 审查修复：连接级频率限制（与 fetch_keys 共享窗口），
    // 防止恶意循环拉取耗尽他人预密钥池（M11：改用 RateWindow，超限返回 RateLimited）
    if (!m_fetchKeysWindow.allow(QDateTime::currentSecsSinceEpoch())) {
        sendResponse(packet.requestId, MessageType::FetchGroupKeysResponse,
                     ErrorCode::RateLimited,
                     "Too many key bundle requests, please slow down");
        return;
    }

    const QList<qint64> memberIds = m_db->getGroupMemberIds(convId);

    QJsonObject bundlesByUser;
    for (qint64 userId : memberIds) {
        // 事务内为每个有库存的设备认领一个预密钥
        const auto claimed = m_db->claimPrekeys(userId);
        if (claimed.isEmpty()) {
            continue;
        }

        QHash<QString, QString> identityByDevice;
        for (const auto &key : m_db->getIdentityKeysByUser(userId)) {
            identityByDevice.insert(key.deviceId, key.identityPub);
        }

        QJsonArray bundles;
        for (const auto &c : claimed) {
            const QString identityPub = identityByDevice.value(c.deviceId);
            if (identityPub.isEmpty()) {
                continue;
            }
            QJsonObject obj;
            obj["deviceId"] = c.deviceId;
            obj["identityPub"] = identityPub;
            obj["prekeyId"] = c.prekeyId;
            obj["prekeyPub"] = c.prekeyPub;
            bundles.append(obj);
        }
        if (!bundles.isEmpty()) {
            bundlesByUser[QString::number(userId)] = bundles;
        }
    }

    QJsonObject data;
    data["conversationId"] = convId;
    data["bundles"] = bundlesByUser;
    sendResponse(packet.requestId, MessageType::FetchGroupKeysResponse, ErrorCode::Ok,
                 "OK", data);
}

// M7a: 群消息发送（明文入库 + fan-out；E2EE 在 M7b 用 Sender Keys 补齐）
void RequestHandler::processSendGroupMessage(const Packet &packet, const QJsonObject &request,
                                             qint64 conversationId, const QString &clientMessageId)
{
    auto conv = m_db->getConversation(conversationId);
    if (!conv.has_value()) {
        sendResponse(packet.requestId, MessageType::SendMessageResponse,
                     ErrorCode::ConversationNotFound, "Conversation not found");
        return;
    }
    if (conv->type != "group") {
        sendResponse(packet.requestId, MessageType::SendMessageResponse,
                     ErrorCode::InvalidRequest,
                     "conversationId refers to a private conversation, use toUserId");
        return;
    }

    // 先授权：仅群成员可发言
    if (!m_db->isConversationMember(conversationId, m_authenticatedUserId)) {
        sendResponse(packet.requestId, MessageType::SendMessageResponse,
                     ErrorCode::PermissionDenied, "Not a member of this group");
        return;
    }

    const QString content = request.value("content").toString();
    const QString contentType = request.value("contentType").toString("text");
    if (content.isEmpty()) {
        sendResponse(packet.requestId, MessageType::SendMessageResponse,
                     ErrorCode::InvalidRequest, "Message content is empty");
        return;
    }
    if (content.size() > MaxGroupMessageLength) {
        sendResponse(packet.requestId, MessageType::SendMessageResponse,
                     ErrorCode::InvalidRequest, "Message content too long");
        return;
    }
    if (contentType != "text"
        && contentType != "sender_key_distribution"
        && contentType != "e2ee_group") {
        sendResponse(packet.requestId, MessageType::SendMessageResponse,
                     ErrorCode::InvalidRequest,
                     "Unsupported group message content type");
        return;
    }

    // 修复：群 Sender Key 分发引用的预密钥须在入库后消费（claimed->used），与单聊
    // processSendMessage 一致；否则 claimed 预密钥 10 分钟超时回收为 unused，被后续
    // fetch_group_keys 以 ORDER BY id 重复 claim，而接收方首次解密已删除本地私钥，
    // 导致轮换后的新 distribution 永远解不开（群消息显示“无法解密”）
    QList<qint64> distPrekeyIds;

    // M7b fail-closed：群 E2EE 消息入库前必须是合法 envelope 密文（与单聊 decodeEnvelope
    // 强校验一致），拒绝明文或结构非法的 blob 伪装成密文入库与 fan-out
    if (contentType == "e2ee_group") {
        GroupE2eeCrypto::EncryptedMessage probe;
        QString senderDeviceId;
        if (!GroupE2eeCrypto::decodeGroupMessage(content, probe, &senderDeviceId)
            || senderDeviceId.isEmpty()) {
            StructuredLogger::event(LogLevel::Warning, "envelope.rejected")
                .requestId(packet.requestId).userId(m_authenticatedUserId)
                .field("reason", "invalid_group_e2ee")
                .field("contentType", "e2ee_group")
                .field("conversationId", conversationId).write();
            sendResponse(packet.requestId, MessageType::SendMessageResponse,
                         ErrorCode::E2eeInvalidEnvelope,
                         "Group e2ee content is not a valid envelope");
            return;
        }
    } else if (contentType == "sender_key_distribution") {
        qint64 distGroupId = 0;
        qint64 distSenderId = 0;
        QString distDeviceId;
        GroupE2eeCrypto::SenderKey distKey;
        QList<GroupE2eeCrypto::DistributionEntry> distEntries;
        if (!GroupE2eeCrypto::decodeDistribution(content, distGroupId, distSenderId,
                                                 distDeviceId, distKey, distEntries)
            || distEntries.isEmpty()
            || distGroupId != conversationId) {
            StructuredLogger::event(LogLevel::Warning, "envelope.rejected")
                .requestId(packet.requestId).userId(m_authenticatedUserId)
                .field("reason", "invalid_sender_key_distribution")
                .field("contentType", "sender_key_distribution")
                .field("conversationId", conversationId).write();
            sendResponse(packet.requestId, MessageType::SendMessageResponse,
                         ErrorCode::E2eeInvalidEnvelope,
                         "sender_key_distribution content is not a valid envelope");
            return;
        }
        // 提取接收方设备条目引用的预密钥（排除发送方自身拷贝 prekeyId=0）
        for (const auto &e : distEntries) {
            if (e.envelope.prekeyId > 0) {
                distPrekeyIds.append(e.envelope.prekeyId);
            }
        }
    }

    // M5.5 幂等重试优先：同键消息已存在时直接返回
    if (auto existing = m_db->getMessageByClientKey(m_authenticatedUserId,
                                                    m_currentDeviceId, clientMessageId);
        existing.has_value()) {
        QJsonObject retryData;
        retryData["messageId"] = existing->id;
        retryData["conversationId"] = existing->conversationId;
        retryData["clientMessageId"] = clientMessageId;
        retryData["status"] = existing->status;
        retryData["reused"] = true;
        sendResponse(packet.requestId, MessageType::SendMessageResponse, ErrorCode::Ok,
                     "Message sent", retryData);
        return;
    }

    const qint64 msgId = m_db->sendMessage(conversationId, m_authenticatedUserId, content,
                                           contentType, clientMessageId, m_currentDeviceId);
    if (msgId < 0) {
        sendResponse(packet.requestId, MessageType::SendMessageResponse,
                     ErrorCode::InternalError, "Failed to send message");
        return;
    }

    // 修复：群分发入库后消费其引用的预密钥（claimed->used），杜绝超时回收后重复 claim
    if (!distPrekeyIds.isEmpty()) {
        m_db->consumePrekeys(distPrekeyIds);
    }

    const QString createdAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    const QString senderUsername = m_db->usernameById(m_authenticatedUserId);

    // 写入全体成员（含发送方其他设备）的同步事件流，离线由游标兜底
    QJsonObject ev;
    ev["messageId"] = msgId;
    ev["conversationId"] = conversationId;
    ev["senderId"] = m_authenticatedUserId;
    ev["content"] = content;
    ev["contentType"] = contentType;
    ev["clientMessageId"] = clientMessageId;
    ev["createdAt"] = createdAt;
    const QString evJson = QJsonDocument(ev).toJson(QJsonDocument::Compact);

    // 在线推送给除发送方外的成员（小群直推 fan-out）
    QJsonObject notifyJson = ev;
    notifyJson["senderUsername"] = senderUsername;
    Packet notifyPacket;
    notifyPacket.messageType = MessageType::NewMessageNotification;
    notifyPacket.requestId = 0;
    notifyPacket.payload = QJsonDocument(notifyJson).toJson(QJsonDocument::Compact);
    const QByteArray encoded = PacketCodec::encode(notifyPacket);

    for (qint64 memberId : m_db->getGroupMemberIds(conversationId)) {
        m_db->appendSyncEvent(memberId, "message", evJson);
        if (memberId != m_authenticatedUserId) {
            emit messageForUser(memberId, encoded);
        }
    }

    QJsonObject data;
    data["messageId"] = msgId;
    data["conversationId"] = conversationId;
    data["clientMessageId"] = clientMessageId;
    data["status"] = "sent";
    sendResponse(packet.requestId, MessageType::SendMessageResponse, ErrorCode::Ok,
                 "Message sent", data);
}

// M9 特性栈：设置会话偏好（置顶/免打扰）。
// 仅会话成员可设置；偏好为本人维度（多端共享同一偏好），写入成员行后
// 向本人所有在线设备实时推送 ConversationPrefsNotification 并写 sync_events 兜底。
void RequestHandler::processSetConversationPrefsRequest(const Packet &packet,
                                                        const QJsonObject &request)
{
    const qint64 conversationId = request.value("conversationId").toVariant().toLongLong();
    if (conversationId <= 0) {
        sendResponse(packet.requestId, MessageType::SetConversationPrefsResponse,
                     ErrorCode::InvalidRequest, "Invalid conversationId");
        return;
    }

    // pinned/muted 接受布尔或 0/1，其余取值视为非法
    const QJsonValue pinnedVal = request.value("pinned");
    const QJsonValue mutedVal = request.value("muted");
    const bool hasPinned = pinnedVal.isBool() || pinnedVal.isDouble();
    const bool hasMuted = mutedVal.isBool() || mutedVal.isDouble();
    if (!hasPinned || !hasMuted) {
        sendResponse(packet.requestId, MessageType::SetConversationPrefsResponse,
                     ErrorCode::InvalidRequest, "pinned and muted must be boolean");
        return;
    }
    const bool pinned = pinnedVal.toBool();
    const bool muted = mutedVal.toBool();

    // M9 欠账修复：会话偏好设置限流（与 send/search 一致：先校验入参形态、
    // 后消费配额，避免畸形请求白白耗尽限流窗口；鉴权已过、DB 访问前）
    if (!m_prefsWindow.allow(QDateTime::currentSecsSinceEpoch())) {
        sendResponse(packet.requestId, MessageType::SetConversationPrefsResponse,
                     ErrorCode::RateLimited, "Too many preference updates, please slow down");
        return;
    }

    // 先授权：仅会话成员可设置偏好
    if (!m_db->isConversationMember(conversationId, m_authenticatedUserId)) {
        sendResponse(packet.requestId, MessageType::SetConversationPrefsResponse,
                     ErrorCode::PermissionDenied, "Not a member of this conversation");
        return;
    }

    if (!m_db->setConversationPrefs(conversationId, m_authenticatedUserId, pinned, muted)) {
        sendResponse(packet.requestId, MessageType::SetConversationPrefsResponse,
                     ErrorCode::InternalError, "Failed to update conversation preferences");
        return;
    }

    // 多端同步：向本人所有设备推送偏好变更，并写 sync_events 兜底离线设备
    QJsonObject payload;
    payload["conversationId"] = conversationId;
    payload["pinned"] = pinned;
    payload["muted"] = muted;
    const QByteArray payloadJson = QJsonDocument(payload).toJson(QJsonDocument::Compact);

    m_db->appendSyncEvent(m_authenticatedUserId, "conversation_prefs",
                          QString::fromUtf8(payloadJson));

    Packet notifyPacket;
    notifyPacket.messageType = MessageType::ConversationPrefsNotification;
    notifyPacket.requestId = 0;
    notifyPacket.payload = payloadJson;
    emit messageForUser(m_authenticatedUserId, PacketCodec::encode(notifyPacket));

    sendResponse(packet.requestId, MessageType::SetConversationPrefsResponse, ErrorCode::Ok,
                 "OK", payload);
}

// M9 特性栈：编辑消息（仅发送者可编辑；正文为重新加密后的 E2EE envelope/群密文）。
// 编辑后向会话全体成员推送 message_edited 事件（sync_events + 实时推送）。
void RequestHandler::processEditMessageRequest(const Packet &packet, const QJsonObject &request)
{
    const qint64 messageId = request.value("messageId").toVariant().toLongLong();
    const QString content = request.value("content").toString();
    const QString contentType = request.value("contentType").toString();

    if (messageId <= 0) {
        sendResponse(packet.requestId, MessageType::EditMessageResponse,
                     ErrorCode::InvalidRequest, "Invalid messageId");
        return;
    }
    if (content.isEmpty() || contentType.isEmpty()) {
        sendResponse(packet.requestId, MessageType::EditMessageResponse,
                     ErrorCode::InvalidRequest, "content and contentType are required");
        return;
    }

    // M9 欠账修复：编辑/删除共用限流（与 send/search 一致：先校验入参形态、
    // 后消费配额；每次编辑按成员数写 sync_events + fan-out，鉴权已过、DB 访问前）
    if (!m_editDeleteWindow.allow(QDateTime::currentSecsSinceEpoch())) {
        sendResponse(packet.requestId, MessageType::EditMessageResponse,
                     ErrorCode::RateLimited, "Too many edits, please slow down");
        return;
    }

    auto msgOpt = m_db->getMessage(messageId);
    if (!msgOpt.has_value()) {
        sendResponse(packet.requestId, MessageType::EditMessageResponse,
                     ErrorCode::MessageNotFound, "Message not found");
        return;
    }

    // 仅发送者可编辑自己的消息
    if (msgOpt->senderId != m_authenticatedUserId) {
        sendResponse(packet.requestId, MessageType::EditMessageResponse,
                     ErrorCode::PermissionDenied, "Only the sender can edit this message");
        return;
    }
    // 已删除消息不可编辑
    if (msgOpt->deleted) {
        sendResponse(packet.requestId, MessageType::EditMessageResponse,
                     ErrorCode::MessageNotFound, "Message has been deleted");
        return;
    }

    // 正文长度与类型合法性：类型必须与消息所属会话形态一致（系统消息不可编辑）
    if (msgOpt->contentType == "system") {
        sendResponse(packet.requestId, MessageType::EditMessageResponse,
                     ErrorCode::PermissionDenied, "System messages cannot be edited");
        return;
    }
    if (content.size() > MaxGroupMessageLength) {
        sendResponse(packet.requestId, MessageType::EditMessageResponse,
                     ErrorCode::InvalidRequest, "Message content too long");
        return;
    }

    // 编辑正文的 contentType 必须保持原形态（私聊 text/envelope，群 e2ee_group），
    // 拒绝借编辑切换形态注入非法内容
    if (contentType != msgOpt->contentType) {
        sendResponse(packet.requestId, MessageType::EditMessageResponse,
                     ErrorCode::InvalidRequest, "contentType must match the original message");
        return;
    }

    // fail-closed 校验：编辑后的正文仍必须是合法密文（服务端只见密文），
    // 与 send_message 的 envelope 强校验保持一致
    if (contentType == "e2ee_group") {
        GroupE2eeCrypto::EncryptedMessage probe;
        QString senderDeviceId;
        if (!GroupE2eeCrypto::decodeGroupMessage(content, probe, &senderDeviceId)
            || senderDeviceId != m_currentDeviceId) {
            StructuredLogger::event(LogLevel::Warning, "envelope.rejected")
                .requestId(packet.requestId).userId(m_authenticatedUserId)
                .field("reason", "invalid_group_e2ee_edit")
                .field("messageId", messageId).write();
            sendResponse(packet.requestId, MessageType::EditMessageResponse,
                         ErrorCode::E2eeInvalidEnvelope,
                         "Edited group e2ee content is not a valid envelope");
            return;
        }
    } else if (contentType == "text") {
        // 私聊正文为 pairwise E2EE envelope；存量明文形态亦允许（fail-closed 只针对
        // 明显非法的结构，与 send_message 的 decodeEnvelope 强校验一致）
        bool envelopeOk = false;
        XYChat::Security::E2eeCrypto::decodeEnvelope(content, &envelopeOk);
        if (!envelopeOk) {
            sendResponse(packet.requestId, MessageType::EditMessageResponse,
                         ErrorCode::E2eeInvalidEnvelope,
                         "Edited content must be a valid E2EE envelope");
            return;
        }
    }

    if (!m_db->editMessage(messageId, content, contentType)) {
        sendResponse(packet.requestId, MessageType::EditMessageResponse,
                     ErrorCode::InternalError, "Failed to edit message");
        return;
    }

    const QString editedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    // 向会话全体成员推送编辑事件（sync_events 兜底 + 实时推送）。
    // senderId 必带：群聊正文为 e2ee_group 密文，接收端需以（群, 发送者, 设备,
    // keyId）定位 Sender Key 才能解密；缺失会使编辑后的群消息在所有接收端不可解。
    // originDeviceId 供发起设备去重（不回显自身操作）。
    QJsonObject ev;
    ev["messageId"] = messageId;
    ev["conversationId"] = msgOpt->conversationId;
    ev["senderId"] = m_authenticatedUserId;
    ev["originDeviceId"] = m_currentDeviceId;
    ev["content"] = content;
    ev["contentType"] = contentType;
    ev["editedAt"] = editedAt;
    const QByteArray evJson = QJsonDocument(ev).toJson(QJsonDocument::Compact);

    Packet notifyPacket;
    notifyPacket.messageType = MessageType::MessageEditedNotification; // M9 欠账修复：专用推送类型
    notifyPacket.requestId = 0;
    notifyPacket.payload = evJson;
    const QByteArray encoded = PacketCodec::encode(notifyPacket);

    for (qint64 memberId : m_db->getConversationMemberIds(msgOpt->conversationId)) {
        m_db->appendSyncEvent(memberId, "message_edited", QString::fromUtf8(evJson));
        // 推送覆盖操作者本人：其名下其他设备同样需要实时一致（onMessageForUser
        // 发给该用户全部会话），发起设备由客户端按 originDeviceId 自行忽略
        emit messageForUser(memberId, encoded);
    }

    sendResponse(packet.requestId, MessageType::EditMessageResponse, ErrorCode::Ok,
                 "OK", ev);
}

// M9 特性栈：删除消息（仅发送者可删；软删除留墓碑）。删除后向会话全体成员
// 推送 message_deleted 事件（sync_events + 实时推送）。
void RequestHandler::processDeleteMessageRequest(const Packet &packet, const QJsonObject &request)
{
    const qint64 messageId = request.value("messageId").toVariant().toLongLong();
    if (messageId <= 0) {
        sendResponse(packet.requestId, MessageType::DeleteMessageResponse,
                     ErrorCode::InvalidRequest, "Invalid messageId");
        return;
    }

    // M9 欠账修复：编辑/删除共用限流（与 send/search 一致：先校验入参形态、
    // 后消费配额；与 edit 同窗口，抑制刷库/O(N) 事件放大；鉴权已过、DB 访问前）
    if (!m_editDeleteWindow.allow(QDateTime::currentSecsSinceEpoch())) {
        sendResponse(packet.requestId, MessageType::DeleteMessageResponse,
                     ErrorCode::RateLimited, "Too many deletions, please slow down");
        return;
    }

    auto msgOpt = m_db->getMessage(messageId);
    if (!msgOpt.has_value()) {
        sendResponse(packet.requestId, MessageType::DeleteMessageResponse,
                     ErrorCode::MessageNotFound, "Message not found");
        return;
    }

    // 仅发送者可删除自己的消息（系统消息不可删）
    if (msgOpt->senderId != m_authenticatedUserId) {
        sendResponse(packet.requestId, MessageType::DeleteMessageResponse,
                     ErrorCode::PermissionDenied, "Only the sender can delete this message");
        return;
    }
    if (msgOpt->contentType == "system") {
        sendResponse(packet.requestId, MessageType::DeleteMessageResponse,
                     ErrorCode::PermissionDenied, "System messages cannot be deleted");
        return;
    }

    // 幂等：已删除消息重复删除仍返回成功（软删除语义）；但真实写入失败时
    // 必须 fail-closed：不得在库内状态未变的情况下向全员广播删除事件
    if (!m_db->deleteMessage(messageId)) {
        StructuredLogger::event(LogLevel::Warning, "message.delete_failed")
            .requestId(packet.requestId).userId(m_authenticatedUserId)
            .field("messageId", messageId).write();
        sendResponse(packet.requestId, MessageType::DeleteMessageResponse,
                     ErrorCode::InternalError, "Failed to delete message");
        return;
    }

    const QString deletedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    QJsonObject ev;
    ev["messageId"] = messageId;
    ev["conversationId"] = msgOpt->conversationId;
    ev["senderId"] = m_authenticatedUserId;
    ev["originDeviceId"] = m_currentDeviceId;
    ev["deletedAt"] = deletedAt;
    const QByteArray evJson = QJsonDocument(ev).toJson(QJsonDocument::Compact);

    Packet notifyPacket;
    notifyPacket.messageType = MessageType::MessageDeletedNotification; // M9 欠账修复：专用推送类型
    notifyPacket.requestId = 0;
    notifyPacket.payload = evJson;
    const QByteArray encoded = PacketCodec::encode(notifyPacket);

    for (qint64 memberId : m_db->getConversationMemberIds(msgOpt->conversationId)) {
        m_db->appendSyncEvent(memberId, "message_deleted", QString::fromUtf8(evJson));
        // 推送覆盖操作者本人（其他设备实时一致），发起设备由客户端按
        // originDeviceId 自行忽略
        emit messageForUser(memberId, encoded);
    }

    sendResponse(packet.requestId, MessageType::DeleteMessageResponse, ErrorCode::Ok,
                 "OK", ev);
}

// Session 验证（P1 安全加固 2026-09-02）
// 除连接级内存态外，逐请求回查 sessions 表并校验客户端携带的 token，
// 消除“token 被终止/过期/续期换代后存量连接仍可通过校验”的漏洞：
// - session 被删除（logout/terminate_session/续期换代）后立即失效；
// - session 过期（expires_at）后拒绝；
// - 请求必须携带与当前 session token 哈希一致的 token（逐包验 token）。
bool RequestHandler::validateSession(const QJsonObject &request)
{
    if (m_currentSessionId <= 0 || m_authenticatedUserId <= 0) {
        return false;
    }

    auto sessionOpt = m_db->getSessionById(m_currentSessionId);
    if (!sessionOpt.has_value()) {
        // session 已不存在：已被登出/终止/续期换代删除
        return false;
    }
    const SessionInfo &session = *sessionOpt;
    if (session.userId != m_authenticatedUserId) {
        StructuredLogger::event(LogLevel::Warning, "session.rejected")
            .requestId(m_currentRequestId).userId(m_authenticatedUserId)
            .field("reason", "session_user_mismatch")
            .field("sessionId", m_currentSessionId).write();
        return false;
    }

    // 过期校验（fail-closed）：expiresAt 格式异常一律拒绝，不放行；
    // token_renew 豁免过期门，允许对已过期会话续期（其余请求过期即拒）
    const QDateTime expiresAt = QDateTime::fromString(session.expiresAt, Qt::ISODate);
    if (!expiresAt.isValid()) {
        StructuredLogger::event(LogLevel::Warning, "session.rejected")
            .requestId(m_currentRequestId).userId(m_authenticatedUserId)
            .field("reason", "unparseable_expires_at")
            .field("sessionId", m_currentSessionId).write();
        return false;
    }
    const bool isRenew = request.value("type").toString() == QLatin1String("token_renew");
    if (!isRenew && expiresAt < QDateTime::currentDateTimeUtc()) {
        return false;
    }

    // 逐包验 token：请求必须携带与 session 记录一致的 token
    const QString suppliedToken = request.value("token").toString();
    if (suppliedToken.isEmpty()) {
        return false;
    }
    if (EncryptionManager::hashToken(suppliedToken) != session.tokenHash) {
        StructuredLogger::event(LogLevel::Warning, "session.rejected")
            .requestId(m_currentRequestId).userId(m_authenticatedUserId)
            .field("reason", "token_mismatch")
            .field("sessionId", m_currentSessionId).write();
        return false;
    }
    return true;
}

// 限流检查
bool RequestHandler::checkRateLimit(const QString &ipAddress, qint64 userId)
{
    const int ipFails = m_db->recentFailedLoginCount(ipAddress, RateLimitWindowSeconds);
    if (ipFails >= MaxFailedLoginsPerIP) {
        StructuredLogger::event(LogLevel::Warning, "auth.rate_limited")
            .requestId(m_currentRequestId).userId(userId)
            .field("scope", "ip").ipField("ip", ipAddress)
            .field("failures", ipFails).write();
        return true;
    }
    if (userId > 0) {
        const int userFails = m_db->recentFailedLoginCountForUser(userId, RateLimitWindowSeconds);
        if (userFails >= MaxFailedLoginsPerUser) {
            StructuredLogger::event(LogLevel::Warning, "auth.rate_limited")
                .requestId(m_currentRequestId).userId(userId)
                .field("scope", "user")
                .field("failures", userFails).write();
            return true;
        }
    }
    return false;
}

// 响应工具
void RequestHandler::sendResponse(quint64 requestId,
                                  MessageType messageType,
                                  ErrorCode code,
                                  const QString &message,
                                  const QJsonObject &data)
{
    QJsonObject response;
    response["code"] = static_cast<int>(code);
    response["message"] = message;
    response["data"] = data;

    Packet packet;
    packet.messageType = messageType;
    packet.requestId = requestId;
    packet.payload = QJsonDocument(response).toJson(QJsonDocument::Compact);
    sendPacket(packet);

    // M11: 结构化审计日志。每个请求响应统一记录请求 ID/用户/设备/类型/错误码/耗时/来源 IP，
    // 成功记为 info、非成功记为 warning，便于统计失败率与延迟；IP 经 LogSanitizer 脱敏，
    // 不记录消息正文/凭据。
    const qint64 durationMs = m_requestTimer.isValid() ? m_requestTimer.elapsed() : 0;
    const QString peerIp = m_socket ? m_socket->peerAddress().toString() : QString();
    StructuredLogger::event(code == ErrorCode::Ok ? LogLevel::Info : LogLevel::Warning,
                            "response")
        .requestId(requestId)
        .userId(m_authenticatedUserId)
        .deviceId(m_currentDeviceId)
        .field("type", m_currentRequestType)
        .errorCode(static_cast<int>(code))
        .durationMs(durationMs)
        .ipField("ip", peerIp)
        .write();
    m_requestTimer.invalidate();
}

void RequestHandler::sendPacket(const Packet &packet)
{
    const QByteArray encoded = PacketCodec::encode(packet);
    if (encoded.isEmpty()) {
        StructuredLogger::event(LogLevel::Warning, "packet.encode_failed")
            .requestId(packet.requestId).write();
        return;
    }

    m_socket->write(encoded);
    m_socket->flush();
}

// M5.5: 重放保护（timestamp/nonce 强制必填）
bool RequestHandler::checkReplayProtection(const QJsonObject &request)
{
    // timestamp 必填且为合法整数
    if (!request.contains("timestamp") || !request.value("timestamp").isDouble()) {
        StructuredLogger::event(LogLevel::Warning, "replay.rejected")
            .requestId(m_currentRequestId).field("reason", "missing_or_invalid_timestamp").write();
        return false;
    }
    const qint64 timestamp = request.value("timestamp").toVariant().toLongLong();
    if (timestamp <= 0) {
        StructuredLogger::event(LogLevel::Warning, "replay.rejected")
            .requestId(m_currentRequestId).field("reason", "non_positive_timestamp").write();
        return false;
    }
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const qint64 diff = qAbs(now - timestamp);
    if (diff > ReplayTimestampToleranceSecs) {
        StructuredLogger::event(LogLevel::Warning, "replay.rejected")
            .requestId(m_currentRequestId).field("reason", "timestamp_out_of_window")
            .field("diffSecs", diff).write();
        return false;
    }

    // nonce 必填：非空、长度受限（不记录 nonce 值本身）
    const QString nonce = request.value("nonce").toString().trimmed();
    if (nonce.isEmpty() || nonce.size() > 128) {
        StructuredLogger::event(LogLevel::Warning, "replay.rejected")
            .requestId(m_currentRequestId).field("reason", "missing_or_oversized_nonce").write();
        return false;
    }

    // 全局 TTL 缓存去重（跨连接生效）；未配置时退回拒绝，fail-closed
    if (!m_nonceCache) {
        StructuredLogger::event(LogLevel::Critical, "replay.rejected")
            .requestId(m_currentRequestId).field("reason", "nonce_cache_unavailable").write();
        return false;
    }
    if (!m_nonceCache->checkAndInsert(nonce)) {
        StructuredLogger::event(LogLevel::Warning, "replay.rejected")
            .requestId(m_currentRequestId).field("reason", "duplicate_nonce").write();
        return false;
    }

    return true;
}
