#include "NetworkManager.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSysInfo>
#include <QDateTime>
#include <QUuid>
#include <QSslConfiguration>
#include <QFile>
#include <QDir>
#include <QSet>
#include <QCoreApplication>
#include <QStandardPaths>

#include "EncryptionManager.h"
#include "TlsHelper.h"
#include "SecureMemory.h"
#include "GroupE2eeCrypto.h"

using namespace XYChat::Protocol;
using namespace XYChat::Security;
using XYChat::Security::GroupE2eeCrypto;

namespace {
// P2: 会话续期策略——会话 TTL 为 7 天，在过期前 1 天（第 6 天）自动续期，
// 避免到期瞬间掉线；续期瞬时失败（网络/服务端内部错误）按 5 分钟退避重试；
// 续期响应超时（60 秒未收到响应）视为丢失，重新触发续期。
constexpr qint64 kRenewBeforeExpirySecs = 86400;   // 过期前 1 天
constexpr int kRenewRetryDelayMs = 300000;          // 瞬时失败退避 5 分钟
constexpr int kRenewResponseTimeoutMs = 60000;      // 续期响应看门狗 60 秒
} // namespace

NetworkManager::NetworkManager(QObject *parent) :
    QObject(parent),
    m_sslSocket(new QSslSocket(this)),
    m_heartbeatTimer(new QTimer(this)),
    m_reconnectTimer(new QTimer(this)),
    m_tokenRenewTimer(new QTimer(this)),
    m_fileTransfer(new XYChat::Client::FileTransferManager(this))
{
    m_heartbeatTimer->setInterval(30000);
    m_reconnectTimer->setInterval(3000);
    m_reconnectTimer->setSingleShot(true);
    m_tokenRenewTimer->setSingleShot(true);

    connect(m_sslSocket, &QSslSocket::connected, this, &NetworkManager::onConnected);
    connect(m_sslSocket, &QSslSocket::disconnected, this, &NetworkManager::onDisconnected);
    connect(m_sslSocket, &QSslSocket::readyRead, this, &NetworkManager::onReadyRead);
    connect(m_sslSocket, &QSslSocket::errorOccurred, this, &NetworkManager::onSocketError);
    connect(m_sslSocket, &QSslSocket::sslErrors, this, &NetworkManager::onSslErrors);
    connect(m_heartbeatTimer, &QTimer::timeout, this, &NetworkManager::sendHeartbeat);
    connect(m_reconnectTimer, &QTimer::timeout, this, &NetworkManager::connectToServer);
    connect(m_tokenRenewTimer, &QTimer::timeout, this, &NetworkManager::renewToken);

    // M5: 初始化 TLS
    initTls();

    // M8.2: 传输引擎的控制面接线。须在 initTls 之后：引擎复用同一份 CA
    // 配置访问 https 数据面，不另行放宽证书校验
    wireFileTransfer();
}

// 登录
void NetworkManager::login(const QString &username, const QString &password)
{
    m_pendingUsername = username;
    m_pendingPassword = password;
    m_loginQueued = true;
    m_registerQueued = false;
    m_reconnectEnabled = true;

    if (m_state == ConnectionState::Connected || m_state == ConnectionState::Authenticated) {
        sendLoginRequest();
        return;
    }

    if (m_state == ConnectionState::Disconnected) {
        connectToServer();
    }
}

// 注册
void NetworkManager::registerAccount(const QString &username, const QString &password,
                                     const QString &email, const QString &phone)
{
    m_pendingUsername = username;
    m_pendingRegisterPassword = password;
    m_pendingEmail = email;
    m_pendingPhone = phone;
    m_registerQueued = true;
    m_loginQueued = false;
    m_reconnectEnabled = false;

    if (m_state == ConnectionState::Connected || m_state == ConnectionState::Authenticated) {
        sendRegisterRequest();
        return;
    }

    if (m_state == ConnectionState::Disconnected) {
        connectToServer();
    }
}

// 登出
void NetworkManager::logout()
{
    if (m_state != ConnectionState::Authenticated) {
        return;
    }

    QJsonObject json;
    json["type"] = "logout";
    addReplayProtection(json);

    Packet packet;
    packet.messageType = MessageType::LogoutRequest;
    packet.requestId = nextRequestId();
    packet.payload = QJsonDocument(json).toJson(QJsonDocument::Compact);
    sendPacket(packet);
}

// Token 续期
void NetworkManager::renewToken()
{
    if (m_state != ConnectionState::Authenticated || m_sessionToken.isEmpty()) {
        return;
    }
    // P2: 若已有在途续期请求，说明是响应超时看门狗触发——清除在途标记并重发，
    // 避免续期响应丢失导致会话静默过期
    if (m_pendingTokenRenewRequestId != 0) {
        qWarning() << "[NetMgr] Token renewal response timeout, retrying";
        m_pendingTokenRenewRequestId = 0;
    }

    QJsonObject json;
    json["type"] = "token_renew";
    json["token"] = m_sessionToken;
    addReplayProtection(json);

    Packet packet;
    packet.messageType = MessageType::TokenRenewRequest;
    packet.requestId = nextRequestId();
    packet.payload = QJsonDocument(json).toJson(QJsonDocument::Compact);
    m_pendingTokenRenewRequestId = packet.requestId;
    sendPacket(packet);

    // 响应看门狗：60 秒内未收到续期响应则重新触发续期（响应丢失兜底）
    if (m_tokenRenewTimer) {
        m_tokenRenewTimer->start(kRenewResponseTimeoutMs);
    }
}

// 连接回调
void NetworkManager::onConnected()
{
    setState(ConnectionState::Connected);
    m_heartbeatTimer->start();

    if (m_loginQueued) {
        sendLoginRequest();
    } else if (m_registerQueued) {
        sendRegisterRequest();
    }
}

void NetworkManager::onDisconnected()
{
    const bool shouldRelogin = m_reconnectEnabled && !m_pendingUsername.isEmpty();

    m_heartbeatTimer->stop();
    // P2: 连接断开时清在途续期请求并停续期定时器（重连重登后由登录响应重新调度）
    if (m_tokenRenewTimer) {
        m_tokenRenewTimer->stop();
    }
    m_pendingTokenRenewRequestId = 0;
    m_codec.reset();
    m_pendingLoginRequestId = 0;
    // M5.5: 在途发送请求随连接丢失，清除映射，
    // 重新登录后由 outbox 以相同幂等键重发（服务端去重）
    m_pendingSendByRequestId.clear();
    // M6: 密钥交换在途状态随连接重置，重新登录后重新引导
    m_e2eeReady = false;
    m_e2eeBootstrapPending = false;
    m_pendingRegisterKeysRequestId = 0;
    m_pendingFetchKeysRequestId = 0;
    m_fetchKeysTargetUserId = 0;
    // M7b: 群 E2EE 引导状态与内存缓存重置（Sender Key 保留在 LocalStore）
    m_pendingFetchGroupKeysRequestId = 0;
    m_fetchGroupKeysTargetConvId = 0;
    m_pendingGroupDistributions.clear();
    m_groupSenderKeys.clear();
    m_healQueue.clear();
    // M9 欠账修复：在途编辑/删除随连接失效。必须复位 m_editFetchInFlight
    // 与清空私聊编辑队列，否则自动重连（不经 resetAuthState）后
    // pumpPrivateEditFetch 首行即因在途标记恒真而 return，本会话所有后续
    // 私聊编辑静默丢失。已发出的编辑/删除响应在断链后不可达，上报失败让 UI
    // 回退乐观态（编辑/删除无 outbox 重发兜底）
    const int pendingEditFailures = m_pendingEdits.size() + m_privateEditQueue.size();
    const int pendingDeleteFailures = m_pendingDeleteRequestIds.size();
    m_pendingEdits.clear();
    // M8.2: 中止在途传输并清零已登记的清单密钥（含文件密钥，不得跨会话驻留）。
    // 登出与断线是两条独立路径，两处都必须清（同 M9 编辑泵的教训）
    m_fileTransfer->reset();
    m_fileTransfer->setBaseUrl(QString());
    m_fileSeqByRequestId.clear();
    m_privateEditQueue.clear();
    m_editFetchInFlight = false;
    m_pendingDeleteRequestIds.clear();
    for (int i = 0; i < pendingEditFailures; ++i) {
        emit messageEditFailed("Connection lost");
    }
    for (int i = 0; i < pendingDeleteFailures; ++i) {
        emit messageDeleteFailed("Connection lost");
    }
    setState(ConnectionState::Disconnected);

    if (shouldRelogin) {
        m_loginQueued = true;
        m_reconnectTimer->start();
    }
}

void NetworkManager::onReadyRead()
{
    m_codec.appendData(m_sslSocket->readAll());

    while (true) {
        Packet packet;
        QString errorMessage;
        const PacketCodec::DecodeStatus status = m_codec.nextPacket(packet, &errorMessage);
        if (status == PacketCodec::DecodeStatus::NeedMoreData) {
            return;
        }
        if (status == PacketCodec::DecodeStatus::InvalidData) {
            emit loginFailed(errorMessage);
            m_sslSocket->disconnectFromHost();
            return;
        }

        handlePacket(packet);
    }
}

void NetworkManager::onSocketError(QAbstractSocket::SocketError socketError)
{
    Q_UNUSED(socketError);

    if (m_state == ConnectionState::Connecting || m_state == ConnectionState::LoggingIn) {
        const QString err = m_sslSocket->errorString();
        if (m_registerQueued) {
            m_registerQueued = false;
            emit registerFailed(err);
        } else {
            m_loginQueued = false;
            emit loginFailed(err);
        }
        setState(ConnectionState::Disconnected);
    }
}

// M5: SSL 错误处理
void NetworkManager::onSslErrors(const QList<QSslError> &errors)
{
    // 证书错误时明确拒绝连接
    QStringList errorStrings;
    for (const auto &err : errors) {
        errorStrings << err.errorString();
    }
    const QString errMsg = "TLS certificate error: " + errorStrings.join("; ");
    qWarning() << "[NetMgr]" << errMsg;

    if (m_registerQueued) {
        m_registerQueued = false;
        emit registerFailed(errMsg);
    } else if (m_loginQueued) {
        m_loginQueued = false;
        emit loginFailed(errMsg);
    }

    m_sslSocket->disconnectFromHost();
    setState(ConnectionState::Disconnected);
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

    // M5.5: fail-closed：TLS 不可用时拒绝连接，
    // 除非显式设置环境变量 XYCHAT_ALLOW_PLAINTEXT=1（仅限开发）
    if (!m_tlsEnabled) {
        if (qEnvironmentVariable("XYCHAT_ALLOW_PLAINTEXT") == "1") {
            qWarning() << "[NetMgr] Connecting in PLAINTEXT development mode."
                       << "Do not use in production.";
        } else {
            const QString err = 
                "TLS unavailable: CA certificate not found. "
                "Refusing plaintext connection (set XYCHAT_ALLOW_PLAINTEXT=1 for development only).";
            qCritical() << "[NetMgr]" << err;
            m_reconnectEnabled = false;
            if (m_loginQueued) {
                m_loginQueued = false;
                emit loginFailed(err);
            } else if (m_registerQueued) {
                m_registerQueued = false;
                emit registerFailed(err);
            }
            return;
        }
    }

    setState(ConnectionState::Connecting);
    if (m_tlsEnabled) {
        m_sslSocket->connectToHostEncrypted("127.0.0.1", 12345);
    } else {
        m_sslSocket->connectToHost("127.0.0.1", 12345);
    }
}

// M5: TLS 初始化
void NetworkManager::initTls()
{
    using namespace XYChat::Security;

    // 查找 CA 证书：优先可执行文件同级 certs 目录，其次 AppData
    QString caCertPath;
    const QStringList searchDirs = {
        QCoreApplication::applicationDirPath() + "/certs",
        QCoreApplication::applicationDirPath() + "/../certs",
        TlsHelper::defaultCertDir()
    };

    for (const auto &dir : searchDirs) {
        const QString candidate = dir + "/ca.crt";
        if (QFile::exists(candidate)) {
            caCertPath = candidate;
            break;
        }
    }

    if (caCertPath.isEmpty()) {
        qWarning() << "[NetMgr] CA certificate not found, TLS disabled (fail-closed on connect)";
        m_tlsUnavailable = true;
        return;
    }

    TlsHelper::TlsConfig config = TlsHelper::loadClientConfig(caCertPath);
    if (!config.valid) {
        qWarning() << "[NetMgr] Failed to load CA certificate, TLS disabled (fail-closed on connect)";
        m_tlsUnavailable = true;
        return;
    }

    // 配置 SSL：添加 CA 证书用于验证服务端
    QSslConfiguration sslConfig = QSslConfiguration::defaultConfiguration();
    sslConfig.addCaCertificate(config.caCertificate);
    sslConfig.setProtocol(QSsl::TlsV1_2OrLater);
    sslConfig.setPeerVerifyMode(QSslSocket::VerifyPeer);
    m_sslSocket->setSslConfiguration(sslConfig);
    m_tlsEnabled = true;
    // M8.2: 数据面（https）用同一套开发 CA，否则自签证书会被全部拒连
    m_fileTransfer->setSslConfiguration(sslConfig);

    qInfo() << "[NetMgr] TLS enabled, CA:" << caCertPath;
}

// 发送登录请求
void NetworkManager::sendLoginRequest()
{
    QJsonObject json;
    json["type"] = "login";
    json["username"] = m_pendingUsername;
    json["password"] = m_pendingPassword;
    json["clientVersion"] = "0.2.0";
    json["platform"] = QSysInfo::productType();
    m_localDeviceId = QString::fromLatin1(QSysInfo::machineUniqueId().toHex());
    json["deviceId"] = m_localDeviceId;
    addReplayProtection(json);

    Packet packet;
    packet.messageType = MessageType::LoginRequest;
    packet.requestId = nextRequestId();
    packet.payload = QJsonDocument(json).toJson(QJsonDocument::Compact);

    m_pendingLoginRequestId = packet.requestId;
    setState(ConnectionState::LoggingIn);
    sendPacket(packet);
}

// 发送注册请求
void NetworkManager::sendRegisterRequest()
{
    QJsonObject json;
    json["type"] = "register";
    json["username"] = m_pendingUsername;
    json["password"] = m_pendingRegisterPassword;
    json["email"] = m_pendingEmail;
    json["phone"] = m_pendingPhone;
    addReplayProtection(json);

    Packet packet;
    packet.messageType = MessageType::RegisterRequest;
    packet.requestId = nextRequestId();
    packet.payload = QJsonDocument(json).toJson(QJsonDocument::Compact);

    m_pendingRegisterRequestId = packet.requestId;
    setState(ConnectionState::LoggingIn);
    sendPacket(packet);
}

// 包分发
void NetworkManager::handlePacket(const Packet &packet)
{
    switch (packet.messageType) {
    case MessageType::LoginResponse:
        handleLoginResponse(packet);
        break;
    case MessageType::RegisterResponse:
        handleRegisterResponse(packet);
        break;
    case MessageType::LogoutResponse:
        handleLogoutResponse(packet);
        break;
    case MessageType::TokenRenewResponse:
        handleTokenRenewResponse(packet);
        break;
    // P1: 服务端鉴权门/校验失败以 MessageType::Error 回包（非对应响应类型）
    case MessageType::Error:
        handleErrorResponse(packet);
        break;
    // M3 响应分发
    case MessageType::SearchUsersResponse:
        handleSearchUsersResponse(packet);
        break;
    case MessageType::AddContactResponse:
        handleAddContactResponse(packet);
        break;
    case MessageType::GetContactsResponse:
        handleGetContactsResponse(packet);
        break;
    case MessageType::GetConversationsResponse:
        handleGetConversationsResponse(packet);
        break;
    case MessageType::SendMessageResponse:
        handleSendMessageResponse(packet);
        break;
    case MessageType::AckMessageResponse:
        handleAckMessageResponse(packet);
        break;
    case MessageType::SyncMessagesResponse:
        handleSyncMessagesResponse(packet);
        break;
    case MessageType::NewMessageNotification:
        handleNewMessageNotification(packet);
        break;
    // M5.5
    case MessageType::MessageStatusUpdate:
        handleMessageStatusUpdate(packet);
        break;
    case MessageType::SyncEventsResponse:
        handleSyncEventsResponse(packet);
        break;
    // M9: 已读游标推送（同账号其他设备已读）
    case MessageType::ReadCursorNotification:
        handleReadCursorNotification(packet);
        break;
    // M6
    case MessageType::RegisterKeysResponse:
        handleRegisterKeysResponse(packet);
        break;
    case MessageType::FetchKeysResponse:
        handleFetchKeysResponse(packet);
        break;
    // M7b: 群 E2EE 密钥包拉取
    case MessageType::FetchGroupKeysResponse:
        handleFetchGroupKeysResponse(packet);
        break;
    // M7a: 群组响应与推送
    case MessageType::CreateGroupResponse:
        handleCreateGroupResponse(packet);
        break;
    case MessageType::InviteGroupMembersResponse:
        handleInviteGroupMembersResponse(packet);
        break;
    case MessageType::LeaveGroupResponse:
        handleLeaveGroupResponse(packet);
        break;
    case MessageType::KickGroupMemberResponse:
        handleKickGroupMemberResponse(packet);
        break;
    case MessageType::GetGroupInfoResponse:
        handleGetGroupInfoResponse(packet);
        break;
    case MessageType::GroupChangedNotification:
        handleGroupChangedNotification(packet);
        break;
    // M9 特性栈：会话偏好与消息编辑/删除
    case MessageType::SetConversationPrefsResponse:
        handleSetConversationPrefsResponse(packet);
        break;
    case MessageType::ConversationPrefsNotification:
        handleConversationPrefsNotification(packet);
        break;
    case MessageType::EditMessageResponse:
        handleEditMessageResponse(packet);
        break;
    case MessageType::DeleteMessageResponse:
        handleDeleteMessageResponse(packet);
        break;
    case MessageType::MessageEditedNotification:
        handleMessageEditedNotification(packet);
        break;
    case MessageType::MessageDeletedNotification:
        handleMessageDeletedNotification(packet);
        break;
    // M8.2: 文件控制面响应（数据面走 HTTP，不经此处）
    case MessageType::FileUploadCreateResponse:
        handleFileUploadCreateResponse(packet);
        break;
    case MessageType::FileUploadQueryResponse:
        handleFileUploadQueryResponse(packet);
        break;
    case MessageType::FileUploadCompleteResponse:
        handleFileUploadCompleteResponse(packet);
        break;
    case MessageType::FileUploadCancelResponse:
        handleFileUploadCancelResponse(packet);
        break;
    case MessageType::FileDownloadTicketResponse:
        handleFileDownloadTicketResponse(packet);
        break;
    case MessageType::Ping: {
        Packet pong;
        pong.messageType = MessageType::Pong;
        pong.requestId = packet.requestId;
        sendPacket(pong);
        break;
    }
    default:
        break;
    }
}

// 登录响应
void NetworkManager::handleLoginResponse(const Packet &packet)
{
    if (packet.requestId != m_pendingLoginRequestId) {
        return;
    }

    const QJsonDocument responseDoc = QJsonDocument::fromJson(packet.payload);
    const QJsonObject response = responseDoc.object();
    const int code = response.value("code").toInt(static_cast<int>(ErrorCode::InternalError));

    m_pendingLoginRequestId = 0;
    if (code == static_cast<int>(ErrorCode::Ok)) {
        const QJsonObject data = response.value("data").toObject();
        m_sessionToken = data.value("token").toString();
        m_userId = data.value("userId").toVariant().toLongLong();
        m_username = data.value("username").toString();
        // P2: 记录会话过期时间并调度自动续期；重置失效通知标记供本轮会话复用
        m_sessionExpiresAtSecs = parseExpiresAt(data.value("expiresAt").toString());
        m_sessionExpiredNotified = false;
        m_loginQueued = false;
        // M8.2: 数据面基地址由服务端下发（而不是客户端猜端口）。字段缺失
        // 意味着服务端未开启文件能力，此时 baseUrl 为空，传输引擎会对任何
        // 任务立即回失败而不是静默排队
        m_fileTransfer->setBaseUrl(data.value("fileTransferBaseUrl").toString());
        setState(ConnectionState::Authenticated);
        emit sessionChanged();
        emit loginSuccessful();
        // M6.5: 打开本地加密缓存（加载持久化 outbox、立即展示缓存会话、游标增量同步）
        openLocalStore();
        // M5.5: 登录成功后重发 outbox 中未确认的消息（幂等键保证不重复）
        flushOutbox();
        scheduleTokenRenew();
        return;
    }

    setState(ConnectionState::Connected);
    const QString message = response.value("message").toString("Unknown error");
    emit loginFailed(message);
}

// 注册响应
void NetworkManager::handleRegisterResponse(const Packet &packet)
{
    if (packet.requestId != m_pendingRegisterRequestId) {
        return;
    }

    const QJsonDocument responseDoc = QJsonDocument::fromJson(packet.payload);
    const QJsonObject response = responseDoc.object();
    const int code = response.value("code").toInt(static_cast<int>(ErrorCode::InternalError));

    m_pendingRegisterRequestId = 0;
    m_registerQueued = false;

    if (code == static_cast<int>(ErrorCode::Ok)) {
        setState(ConnectionState::Connected);
        emit registerSuccessful();
        return;
    }

    setState(ConnectionState::Connected);
    const QString message = response.value("message").toString("Unknown error");
    emit registerFailed(message);
}

// 登出响应
void NetworkManager::handleLogoutResponse(const Packet &packet)
{
    Q_UNUSED(packet);
    resetAuthState();
    m_reconnectEnabled = false;
    setState(ConnectionState::Connected);
    emit logoutFinished();
}

// Token 续期响应
void NetworkManager::handleTokenRenewResponse(const Packet &packet)
{
    // P2: 仅处理当前在途续期请求的响应，避免陈旧响应串扰
    if (packet.requestId != m_pendingTokenRenewRequestId) {
        return;
    }
    m_pendingTokenRenewRequestId = 0;

    const QJsonDocument responseDoc = QJsonDocument::fromJson(packet.payload);
    const QJsonObject response = responseDoc.object();
    const int code = response.value("code").toInt(static_cast<int>(ErrorCode::InternalError));

    if (code == static_cast<int>(ErrorCode::Ok)) {
        const QJsonObject data = response.value("data").toObject();
        // P2: 先擦除旧 token 再替换新 token，与 resetAuthState 的安全清零约定一致
        XYChat::Security::SecureMemory::wipe(m_sessionToken);
        m_sessionToken = data.value("token").toString();
        m_sessionExpiresAtSecs = parseExpiresAt(data.value("expiresAt").toString());
        emit sessionChanged();
        qInfo() << "[NetMgr] Token renewed";
        scheduleTokenRenew();
        return;
    }

    // P2: 续期被拒——token 已失效（被 terminate_session/登出/续期换代删除，或 token 不匹配），
    // 无法自动恢复，回登录页让用户重新登录。
    if (code == static_cast<int>(ErrorCode::SessionInvalid)
        || code == static_cast<int>(ErrorCode::SessionExpired)) {
        qWarning() << "[NetMgr] Token renewal rejected, session invalid";
        notifySessionExpired();
        return;
    }

    // 其他瞬时失败（网络/服务端内部错误）：短退避后重试一次，避免会话无谓失效
    qWarning() << "[NetMgr] Token renewal failed, will retry:"
               << response.value("message").toString();
    if (m_tokenRenewTimer) {
        m_tokenRenewTimer->start(kRenewRetryDelayMs);
    }
}

// 工具
void NetworkManager::sendPacket(const Packet &packet)
{
    const QByteArray encoded = PacketCodec::encode(packet);
    if (encoded.isEmpty()) {
        emit loginFailed("Failed to encode request");
        return;
    }

    m_sslSocket->write(encoded);
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

void NetworkManager::resetAuthState()
{
    // M5: 安全清除敏感数据
    XYChat::Security::SecureMemory::wipe(m_sessionToken);
    XYChat::Security::SecureMemory::wipe(m_pendingPassword);
    XYChat::Security::SecureMemory::wipe(m_pendingRegisterPassword);
    // P2: 会话续期状态重置（停止续期定时器、清在途续期请求与过期时间）
    if (m_tokenRenewTimer) {
        m_tokenRenewTimer->stop();
    }
    m_pendingTokenRenewRequestId = 0;
    m_sessionExpiresAtSecs = 0;
    m_userId = 0;
    m_username.clear();
    m_pendingUsername.clear();
    m_e2eeReady = false;
    m_e2eeBootstrapPending = false;
    m_pendingRegisterKeysRequestId = 0;
    m_pendingFetchKeysRequestId = 0;
    m_fetchKeysTargetUserId = 0;
    m_fetchBackoffUntil.clear();
    m_serverPrekeyRemaining = -1;
    m_decryptCache.clear();
    m_decryptCacheLoaded = false;
    m_pendingFetchGroupKeysRequestId = 0;
    m_fetchGroupKeysTargetConvId = 0;
    m_pendingGroupDistributions.clear();
    m_groupSenderKeys.clear();
    m_healQueue.clear();
    // M9 欠账修复：清空在途编辑/删除状态与等待密钥的私聊编辑队列
    m_pendingEdits.clear();
    // M8.2: 中止在途传输并清零已登记的清单密钥（含文件密钥，不得跨会话驻留）。
    // 登出与断线是两条独立路径，两处都必须清（同 M9 编辑泵的教训）
    m_fileTransfer->reset();
    m_fileTransfer->setBaseUrl(QString());
    m_fileSeqByRequestId.clear();
    m_privateEditQueue.clear();
    m_editFetchInFlight = false;
    m_pendingDeleteRequestIds.clear();
    XYChat::Security::SecureMemory::wipe(m_identityKey.privateKey);
    m_identityKey = {};
    for (auto &pk : m_localPrekeys) {
        XYChat::Security::SecureMemory::wipe(pk.privateKey);
    }
    m_localPrekeys.clear();
    // M6.5: 登出清除本地用户数据（消息/会话/outbox/同步游标）；
    // 解密缓存与存储密钥属 E2EE 密钥材料，必须保留：登出重登时一次性
    // 预密钥已消费不可恢复，对方消息只能靠解密缓存兜底（M6 产品承诺）；
    // E2EE 身份密钥同样不在此列，仍由 KeyStorage 保留供下次登录复用
    if (m_localStore.isOpen()) {
        m_localStore.clearUserData();
        m_localStore.close();
    }
    emit sessionChanged();
}

// P2: 调度自动续期——在会话过期前 kRenewBeforeExpirySecs 触发一次 renewToken()
void NetworkManager::scheduleTokenRenew()
{
    if (!m_tokenRenewTimer) {
        return;
    }
    m_tokenRenewTimer->stop();
    // 未获取到过期时间（旧服务端未返回 expiresAt）时不做调度，避免误触发
    if (m_sessionExpiresAtSecs <= 0) {
        return;
    }

    const qint64 now = QDateTime::currentDateTimeUtc().toSecsSinceEpoch();
    qint64 delayMs = (m_sessionExpiresAtSecs - kRenewBeforeExpirySecs - now) * 1000;
    if (delayMs < 1000) {
        // 已过触发点但尚未过期（或时间漂移）：稍后立即续期一次
        delayMs = 1000;
    }
    // QTimer 以 int 毫秒计，超长间隔（>24.8 天）夹取到上限；实际 6 天远小于上限
    m_tokenRenewTimer->start(static_cast<int>(qMin<qint64>(delayMs, 2147483647LL)));
}

// P2: 会话失效——安全清理并回登录页。幂等：单次会话仅通知一次，避免重复弹窗。
void NetworkManager::notifySessionExpired()
{
    if (m_sessionExpiredNotified) {
        return;
    }
    m_sessionExpiredNotified = true;

    if (m_tokenRenewTimer) {
        m_tokenRenewTimer->stop();
    }
    m_pendingTokenRenewRequestId = 0;
    m_sessionExpiresAtSecs = 0;

    // 安全清零 token 与本地用户数据（保留 E2EE 密钥材料），并停止重连/重登录
    resetAuthState();
    m_loginQueued = false;
    m_registerQueued = false;
    m_reconnectEnabled = false;

    // 断开连接，避免残留无效长连接继续心跳
    if (m_sslSocket->state() != QAbstractSocket::UnconnectedState) {
        m_sslSocket->disconnectFromHost();
    }
    setState(ConnectionState::Disconnected);

    qInfo() << "[NetMgr] Session expired, returning to login";
    emit sessionExpired();
}

// P2: 解析服务端返回的过期时间（UTC ISO 字符串）。
// 服务端以 UTC 生成 ISO 时间；Qt 对无时区后缀的 ISO 串按本地时间解析，
// 此处加回本地时区偏移以还原真实 UTC 时刻，避免续期定时器提前/滞后触发。
qint64 NetworkManager::parseExpiresAt(const QString &iso) const
{
    if (iso.isEmpty()) {
        return 0;
    }
    QDateTime dt = QDateTime::fromString(iso, Qt::ISODate);
    if (!dt.isValid()) {
        return 0;
    }
    qint64 epochSecs = dt.toSecsSinceEpoch();
    if (dt.timeSpec() == Qt::LocalTime) {
        epochSecs += dt.offsetFromUtc();
    }
    return epochSecs;
}

// M5: 重放保护
void NetworkManager::addReplayProtection(QJsonObject &json)
{
    json["timestamp"] = QDateTime::currentSecsSinceEpoch();
    json["nonce"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
    // P1-2 逐包验 token：仅在已认证状态携带当前 session token，
    // 服务端 validateSession 逐包校验其哈希与 sessions 表记录一致。
    // 认证态判断避免登录/注册（认证前）与重连重登路径把 stale token 写入报文。
    if (m_state == ConnectionState::Authenticated && !m_sessionToken.isEmpty()) {
        json["token"] = m_sessionToken;
    }
}

// M3: 用户搜索
void NetworkManager::searchUsers(const QString &query)
{
    if (m_state != ConnectionState::Authenticated) return;

    QJsonObject json;
    json["type"] = "search_users";
    json["query"] = query;
    addReplayProtection(json);

    Packet packet;
    packet.messageType = MessageType::SearchUsersRequest;
    packet.requestId = nextRequestId();
    packet.payload = QJsonDocument(json).toJson(QJsonDocument::Compact);
    m_pendingSearchRequestId = packet.requestId;
    sendPacket(packet);
}

// M3: 添加联系人
void NetworkManager::addContact(qint64 userId)
{
    if (m_state != ConnectionState::Authenticated) return;

    QJsonObject json;
    json["type"] = "add_contact";
    json["userId"] = userId;
    addReplayProtection(json);

    Packet packet;
    packet.messageType = MessageType::AddContactRequest;
    packet.requestId = nextRequestId();
    packet.payload = QJsonDocument(json).toJson(QJsonDocument::Compact);
    m_pendingAddContactRequestId = packet.requestId;
    sendPacket(packet);
}

// M3: 获取联系人列表
void NetworkManager::getContacts()
{
    if (m_state != ConnectionState::Authenticated) return;

    QJsonObject json;
    json["type"] = "get_contacts";
    addReplayProtection(json);

    Packet packet;
    packet.messageType = MessageType::GetContactsRequest;
    packet.requestId = nextRequestId();
    packet.payload = QJsonDocument(json).toJson(QJsonDocument::Compact);
    m_pendingGetContactsRequestId = packet.requestId;
    sendPacket(packet);
}

// M3: 获取会话列表
void NetworkManager::getConversations()
{
    if (m_state != ConnectionState::Authenticated) return;

    QJsonObject json;
    json["type"] = "get_conversations";
    addReplayProtection(json);

    Packet packet;
    packet.messageType = MessageType::GetConversationsRequest;
    packet.requestId = nextRequestId();
    packet.payload = QJsonDocument(json).toJson(QJsonDocument::Compact);
    m_pendingGetConversationsRequestId = packet.requestId;
    sendPacket(packet);
}

// M3: 发送消息
QString NetworkManager::sendMessage(qint64 toUserId, const QString &content, qint64 fileId)
{
    // M5.5: 客户端生成幂等键，重试/重连重发不会产生重复消息
    // M4.5: 返回幂等键供 QML 跟踪乐观消息气泡状态
    const QString clientMessageId = QUuid::createUuid().toString(QUuid::WithoutBraces);

    // M8.2: 文件消息的正文是清单 JSON（含 32 字节文件密钥），而持久化 outbox 表
    // 不带 fileId 列。若把它落库，重启后重发会以 fileId=0 投出一条"正文是清单"
    // 的普通消息，等于把密钥当文本发给接收方。因此文件消息只进内存 outbox：
    // 应用重启后需重新上传（本地源文件仍在），已登记为欠账
    if (fileId <= 0) {
        // M6.5: outbox 加密落库，重启后不丢未发送消息
        if (!m_localStore.isOpen()) {
            // 登录前排队的场景：尝试以待登录账号打开本地库
            const QString user = m_state == ConnectionState::Authenticated
                ? m_username : m_pendingUsername;
            const QString deviceId = m_localDeviceId.isEmpty()
                ? QString::fromLatin1(QSysInfo::machineUniqueId().toHex())
                : m_localDeviceId;
            if (!user.isEmpty() && !deviceId.isEmpty()) {
                m_localStore.open(user, deviceId);
            }
        }
        m_localStore.addOutboxItem(clientMessageId, toUserId, content);
    }

    if (m_state != ConnectionState::Authenticated) {
        // M5.5: 未认证时进入 outbox，登录成功后自动重发
        m_outbox.append({clientMessageId, toUserId, 0, content, fileId});
        return clientMessageId;
    }

    m_outbox.append({clientMessageId, toUserId, 0, content, fileId});
    flushOutbox();
    return clientMessageId;
}

// M7a: 发送群消息（明文；返回幂等键供 QML 乐观消息跟踪）
QString NetworkManager::sendGroupMessage(qint64 conversationId, const QString &content,
                                         qint64 fileId)
{
    if (conversationId <= 0 || content.trimmed().isEmpty()) {
        emit messageSendFailed("Invalid group message");
        return {};
    }

    const QString clientMessageId = QUuid::createUuid().toString(QUuid::WithoutBraces);

    // 与私聊一致：outbox 加密落库，重启后不丢未发送消息。
    // 文件消息例外（只进内存 outbox），理由同 sendMessage
    if (fileId <= 0) {
        if (!m_localStore.isOpen()) {
            const QString user = m_state == ConnectionState::Authenticated
                ? m_username : m_pendingUsername;
            const QString deviceId = m_localDeviceId.isEmpty()
                ? QString::fromLatin1(QSysInfo::machineUniqueId().toHex())
                : m_localDeviceId;
            if (!user.isEmpty() && !deviceId.isEmpty()) {
                m_localStore.open(user, deviceId);
            }
        }
        m_localStore.addOutboxItem(clientMessageId, 0, content, conversationId);
    }

    m_outbox.append({clientMessageId, 0, conversationId, content, fileId});
    if (m_state == ConnectionState::Authenticated) {
        flushOutbox();
    }
    return clientMessageId;
}

// M5.5: 将 outbox 中未确认的消息逐条发送（同一 clientMessageId 只保留一份）
// M6: 发送前先拉取接收方密钥包，正文加密为 envelope 后再提交
void NetworkManager::flushOutbox()
{
    if (m_state != ConnectionState::Authenticated) {
        return;
    }

    // M6/M7b: 身份密钥尚未注册时先引导，完成后会再次 flush
    if (!m_e2eeReady) {
        bootstrapE2ee();
        return;
    }

    // 已在途的 clientMessageId 不重复发
    QSet<QString> inFlight;
    for (auto it = m_pendingSendByRequestId.constBegin();
         it != m_pendingSendByRequestId.constEnd(); ++it) {
        inFlight.insert(it.value());
    }

    // M7b: 群消息使用 Sender Key E2EE
    for (const OutboxItem &item : std::as_const(m_outbox)) {
        if (item.conversationId <= 0 || inFlight.contains(item.clientMessageId)) {
            continue;
        }
        GroupE2eeCrypto::SenderKey key;
        if (ensureGroupSenderKey(item.conversationId, key)) {
            const QString envelope = encryptGroupMessage(item.conversationId, item.content);
            if (envelope.isEmpty()) {
                qWarning() << "[NetMgr] Failed to encrypt group message"
                           << item.clientMessageId;
                continue;
            }
            QJsonObject json;
            json["type"] = "send_message";
            json["conversationId"] = item.conversationId;
            json["content"] = envelope;
            json["contentType"] = "e2ee_group";
            json["clientMessageId"] = item.clientMessageId;
            // M8.2: 文件消息带上 files.id（服务端据此校验归属与 ready 状态，
            // 并随消息同步给接收端；缺它会把清单当普通文本投递）
            if (item.fileId > 0) {
                json["fileId"] = item.fileId;
            }
            addReplayProtection(json);

            Packet groupPacket;
            groupPacket.messageType = MessageType::SendMessageRequest;
            groupPacket.requestId = nextRequestId();
            groupPacket.payload = QJsonDocument(json).toJson(QJsonDocument::Compact);
            m_pendingSendByRequestId.insert(groupPacket.requestId, item.clientMessageId);
            sendPacket(groupPacket);
        } else if (m_pendingFetchGroupKeysRequestId == 0) {
            // 本群尚无 Sender Key：先拉取成员密钥包，响应后继续 flush
            sendFetchGroupKeysRequest(item.conversationId);
            return;
        }
    }

    // 按目标用户汇总待发消息，逐用户拉取密钥包（每次 FetchKeys 的预密钥
    // 仅供一条消息使用，后续消息在响应回调中继续触发 flush）；
    // 处于退避期的目标（对方尚未注册密钥等）暂不拉取
    const qint64 nowSecs = QDateTime::currentSecsSinceEpoch();
    QSet<qint64> targets;
    for (const OutboxItem &item : std::as_const(m_outbox)) {
        if (item.conversationId > 0) {
            continue; // M7a: 群消息已在上方直发
        }
        if (!inFlight.contains(item.clientMessageId)) {
            targets.insert(item.toUserId);
        }
    }
    for (qint64 target : targets) {
        if (m_pendingFetchKeysRequestId != 0) {
            break; // 同一时刻只保持一个在途 FetchKeys，响应后继续
        }
        if (m_fetchBackoffUntil.value(target, 0) > nowSecs) {
            continue; // 等待对方注册密钥，稍后由定时器重试
        }
        sendFetchKeysRequest(target);
    }

    // M9 欠账修复：outbox 拉取若未占用 fetch 传输槽（无待发或均在退避），
    // 在此收口处泵送等待密钥的私聊编辑，避免编辑因“槽被发送链路占用后
    // 无人再泵”而被无限期搁置（pump 内部再判槽空闲，无重入风险）
    pumpPrivateEditFetch();
}

// M6: E2EE 引导（加载/生成身份密钥，补齐预密钥，注册到服务端）
void NetworkManager::bootstrapE2ee()
{
    using namespace XYChat::Security;

    if (m_e2eeBootstrapPending || m_localDeviceId.isEmpty() || m_username.isEmpty()) {
        return;
    }

    // 加载或生成本机身份密钥对
    if (!m_identityKey.valid) {
        QByteArray priv = KeyStorage::loadIdentityPrivateKey(m_username, m_localDeviceId);
        if (priv.isEmpty()) {
            m_identityKey = E2eeCrypto::generateX25519KeyPair();
            if (!m_identityKey.valid) {
                qCritical() << "[NetMgr] Failed to generate identity keypair";
                return;
            }
            if (!KeyStorage::saveIdentityPrivateKey(m_username, m_localDeviceId,
                                                    m_identityKey.privateKey)) {
                // 持久化失败时密钥仅存在于内存，重启后重新生成（服务端会
                // 因身份变更废弃旧预密钥，语义上仍然安全）
                qWarning() << "[NetMgr] Failed to persist identity key,"
                              "it will be regenerated on next launch";
            }
            // 新身份世代：旧预密钥与新身份不匹配，全部丢弃
            for (auto &pk : m_localPrekeys) {
                SecureMemory::wipe(pk.privateKey);
            }
            m_localPrekeys.clear();
            qInfo() << "[NetMgr] Generated new E2EE identity key";
        } else {
            m_identityKey = E2eeCrypto::keyPairFromPrivateKey(priv);
            SecureMemory::wipe(priv);
            if (!m_identityKey.valid) {
                // 审查修复：存储的身份密钥损坏时重新生成，避免发送功能永久阻塞
                qWarning() << "[NetMgr] Stored identity key invalid, regenerating";
                m_identityKey = E2eeCrypto::generateX25519KeyPair();
                if (!m_identityKey.valid) {
                    qCritical() << "[NetMgr] Failed to regenerate identity keypair";
                    return;
                }
                KeyStorage::saveIdentityPrivateKey(m_username, m_localDeviceId,
                                                   m_identityKey.privateKey);
                for (auto &pk : m_localPrekeys) {
                    SecureMemory::wipe(pk.privateKey);
                }
                m_localPrekeys.clear();
            }
        }
    }

    // 加载本地预密钥，低于阈值时补齐并随注册一并上传公钥
    if (m_localPrekeys.isEmpty()) {
        m_localPrekeys = KeyStorage::loadPrekeys(m_username, m_localDeviceId);
    }

    // 加载持久化解密缓存：一次性预密钥解密后即删除，重新登录后
    // 历史消息依靠此缓存恢复明文（修复：登出重登后无法解密旧消息）
    // M6.5: 缓存已归口 LocalStore，仅本地库不可用时回退 KeyStorage 文件
    if (!m_decryptCacheLoaded) {
        if (!m_localStore.isOpen()) {
            m_decryptCache = KeyStorage::loadDecryptCache(m_username, m_localDeviceId);
        }
        m_decryptCacheLoaded = true;
    }

    m_e2eeBootstrapPending = true;
    sendRegisterKeysRequest();
}

void NetworkManager::sendRegisterKeysRequest()
{
    using namespace XYChat::Security;

    QJsonObject json;
    json["type"] = "register_keys";
    json["identityPub"] = QString::fromLatin1(m_identityKey.publicKey.toBase64());

    // 预密钥补齐：同时参考本地存量与服务端报告余量（服务端消费/废弃对
    // 本地不可见，仅看本地会导致服务端枯竭后永不补齐）
    constexpr int PrekeyTarget = 20;
    constexpr int PrekeyLowWatermark = 5;
    const bool localLow = m_localPrekeys.size() < PrekeyLowWatermark;
    const bool serverLow = m_serverPrekeyRemaining >= 0
        && m_serverPrekeyRemaining < PrekeyLowWatermark;
    if (localLow || serverLow) {
        QJsonArray pubs;
        const int uploadCount = PrekeyTarget - (serverLow ? m_serverPrekeyRemaining
                                                          : m_localPrekeys.size());
        for (int i = 0; i < uploadCount; ++i) {
            const auto kp = E2eeCrypto::generateX25519KeyPair();
            if (!kp.valid) {
                break;
            }
            KeyStorage::PrekeyEntry entry;
            entry.publicKey = kp.publicKey;
            entry.privateKey = kp.privateKey;
            m_localPrekeys.append(entry);
            pubs.append(QString::fromLatin1(kp.publicKey.toBase64()));
        }
        KeyStorage::savePrekeys(m_username, m_localDeviceId, m_localPrekeys);
        json["prekeys"] = pubs;
    }

    addReplayProtection(json);

    Packet packet;
    packet.messageType = MessageType::RegisterKeysRequest;
    packet.requestId = nextRequestId();
    packet.payload = QJsonDocument(json).toJson(QJsonDocument::Compact);
    m_pendingRegisterKeysRequestId = packet.requestId;
    sendPacket(packet);
}

void NetworkManager::handleRegisterKeysResponse(const Packet &packet)
{
    if (packet.requestId != m_pendingRegisterKeysRequestId) {
        return;
    }
    m_pendingRegisterKeysRequestId = 0;
    m_e2eeBootstrapPending = false;

    const QJsonObject response = QJsonDocument::fromJson(packet.payload).object();
    if (response.value("code").toInt() == static_cast<int>(ErrorCode::Ok)) {
        m_e2eeReady = true;
        const QJsonObject data = response.value("data").toObject();
        m_serverPrekeyRemaining = data.value("remainingPrekeys").toInt();
        qInfo() << "[NetMgr] E2EE keys registered, remaining prekeys:"
                << m_serverPrekeyRemaining;
        // 引导完成后继续发送 outbox
        flushOutbox();
        // P1-3: E2EE 就绪后排空离线期间排队的群 healing（登录时 sync_events 早于本响应）
        drainHealQueue();
    } else {
        qWarning() << "[NetMgr] RegisterKeys failed:"
                   << response.value("message").toString() << ", retry in 3s";
        QTimer::singleShot(3000, this, [this]() {
            if (m_state == ConnectionState::Authenticated && !m_e2eeReady) {
                bootstrapE2ee();
            }
        });
    }
}

void NetworkManager::sendFetchKeysRequest(qint64 toUserId)
{
    QJsonObject json;
    json["type"] = "fetch_keys";
    json["userId"] = toUserId;
    addReplayProtection(json);

    Packet packet;
    packet.messageType = MessageType::FetchKeysRequest;
    packet.requestId = nextRequestId();
    packet.payload = QJsonDocument(json).toJson(QJsonDocument::Compact);
    m_pendingFetchKeysRequestId = packet.requestId;
    m_fetchKeysTargetUserId = toUserId;
    sendPacket(packet);
}

void NetworkManager::handleFetchKeysResponse(const Packet &packet)
{
    using namespace XYChat::Security;

    if (packet.requestId != m_pendingFetchKeysRequestId) {
        return;
    }
    const qint64 target = m_fetchKeysTargetUserId;
    m_pendingFetchKeysRequestId = 0;
    m_fetchKeysTargetUserId = 0;

    const QJsonObject response = QJsonDocument::fromJson(packet.payload).object();
    const int code = response.value("code").toInt();
    if (code != static_cast<int>(ErrorCode::Ok)) {
        const QString message = response.value("message").toString("Key bundle unavailable");
        // M9 欠账修复：在途私聊编辑拉取密钥包失败——出队上报编辑失败并泵送下一条
        if (m_editFetchInFlight && !m_privateEditQueue.isEmpty()
            && m_privateEditQueue.head().peerUserId == target) {
            m_editFetchInFlight = false;
            m_privateEditQueue.dequeue();
            emit messageEditFailed(message);
            flushOutbox();
            pumpPrivateEditFetch();
            return;
        }
        if (code == static_cast<int>(ErrorCode::CannotSendToSelf)) {
            // 确定性失败：移除该用户的待发项（含持久化 outbox）并上报
            for (int i = m_outbox.size() - 1; i >= 0; --i) {
                if (m_outbox.at(i).toUserId == target) {
                    m_localStore.removeOutboxItem(m_outbox.at(i).clientMessageId);
                    m_outbox.removeAt(i);
                }
            }
            emit messageSendFailed(message);
        } else if (code == static_cast<int>(ErrorCode::KeyBundleUnavailable)
                   || code == static_cast<int>(ErrorCode::AccountNotFound)) {
            // 修复：对方尚未注册 E2EE 密钥（从未登录/未上线）是产品上的
            // 可恢复状态，保留 outbox 并定期重试，对方首次登录注册密钥后送达；
            // 不能直接丢弃消息（此前行为导致离线发送永久丢失）
            qInfo() << "[NetMgr] Target" << target << "has no keys yet,"
                    << "keeping outbox and retrying in 30s:" << message;
            m_fetchBackoffUntil.insert(target, QDateTime::currentSecsSinceEpoch() + 30);
            QTimer::singleShot(30000, this, [this]() { flushOutbox(); });
        } else {
            // 瞬时失败（限流/内部错误等）：保留 outbox，短退避后重试
            qWarning() << "[NetMgr] FetchKeys transient failure:" << message
                       << ", retry in 3s";
            m_fetchBackoffUntil.insert(target, QDateTime::currentSecsSinceEpoch() + 3);
            QTimer::singleShot(3000, this, [this]() { flushOutbox(); });
        }
        flushOutbox();
        return;
    }

    m_fetchBackoffUntil.remove(target);

    const QJsonArray bundles = response.value("data").toObject().value("bundles").toArray();
    if (bundles.isEmpty()) {
        // M9 欠账修复：若为在途私聊编辑拉取到空密钥包，同样出队失败并泵送下一条
        if (m_editFetchInFlight && !m_privateEditQueue.isEmpty()
            && m_privateEditQueue.head().peerUserId == target) {
            m_editFetchInFlight = false;
            m_privateEditQueue.dequeue();
            emit messageEditFailed("Key bundle unavailable");
            pumpPrivateEditFetch();
            return;
        }
        emit messageSendFailed("Empty key bundle");
        flushOutbox();
        return;
    }

    // M9 欠账修复：在途私聊编辑——优先用本轮密钥包加密队首编辑正文并提交，
    // 而非走 outbox 发送链路（fetch 单槽串行，消费后泵送下一条）
    if (m_editFetchInFlight && !m_privateEditQueue.isEmpty()
        && m_privateEditQueue.head().peerUserId == target) {
        m_editFetchInFlight = false;
        const PrivateEditWait w = m_privateEditQueue.dequeue();
        const QString envelope = encryptForUser(target, bundles, w.plaintext);
        if (envelope.isEmpty()) {
            emit messageEditFailed("Failed to encrypt edited message");
        } else {
            sendEditMessageRequest(w.messageId, w.conversationId, envelope, "text", w.plaintext);
        }
        pumpPrivateEditFetch();
        return;
    }

    // TOFU：首次记录对方身份公钥指纹，变更时告警（不阻塞发送）
    {
        const QByteArray identityPub =
            QByteArray::fromBase64(bundles.first().toObject()
                                       .value("identityPub").toString().toLatin1());
        const QString fingerprint = E2eeCrypto::publicKeyFingerprint(identityPub);
        const QString stored = KeyStorage::loadPeerFingerprint(target);
        if (stored.isEmpty()) {
            KeyStorage::savePeerFingerprint(target, fingerprint);
        } else if (stored != fingerprint) {
            qWarning() << "[NetMgr] Peer identity key changed for user" << target;
            KeyStorage::savePeerFingerprint(target, fingerprint);
            emit peerIdentityChanged(target);
        }
    }

    // 已在途的 clientMessageId 不重复发
    QSet<QString> inFlight;
    for (auto it = m_pendingSendByRequestId.constBegin();
         it != m_pendingSendByRequestId.constEnd(); ++it) {
        inFlight.insert(it.value());
    }

    // 认领的预密钥仅供一条消息使用：本轮只处理该用户的第一条待发项，
    // 发送完成后 flushOutbox 会为下一条重新拉取密钥包
    for (int i = 0; i < m_outbox.size(); ++i) {
        const OutboxItem &item = m_outbox.at(i);
        if (item.toUserId != target || inFlight.contains(item.clientMessageId)) {
            continue;
        }

        const QString envelope = encryptForUser(target, bundles, item.content);
        if (envelope.isEmpty()) {
            qWarning() << "[NetMgr] E2EE encryption failed for message"
                       << item.clientMessageId;
            m_localStore.removeOutboxItem(item.clientMessageId);
            m_outbox.removeAt(i);
            emit messageSendFailed("End-to-end encryption failed");
            break;
        }

        QJsonObject json;
        json["type"] = "send_message";
        json["toUserId"] = item.toUserId;
        json["content"] = envelope;
        json["contentType"] = "text";
        json["clientMessageId"] = item.clientMessageId;
        // M8.2: 同群聊路径，文件消息带上 files.id
        if (item.fileId > 0) {
            json["fileId"] = item.fileId;
        }
        addReplayProtection(json);

        Packet sendPacketMsg;
        sendPacketMsg.messageType = MessageType::SendMessageRequest;
        sendPacketMsg.requestId = nextRequestId();
        sendPacketMsg.payload = QJsonDocument(json).toJson(QJsonDocument::Compact);
        m_pendingSendByRequestId.insert(sendPacketMsg.requestId, item.clientMessageId);
        sendPacket(sendPacketMsg);
        break;
    }

    // 继续处理其他目标用户的待发消息
    flushOutbox();
}

// 逐设备加密：每条消息生成临时密钥对，shared = ECDH(eph, prekey) || ECDH(eph, identity)
QString NetworkManager::encryptForUser(qint64 toUserId, const QJsonArray &bundles,
                                       const QString &plaintext)
{
    Q_UNUSED(toUserId);
    using namespace XYChat::Security;

    const QByteArray plainBytes = plaintext.toUtf8();
    QList<E2eeCrypto::EnvelopeEntry> entries;

    for (const QJsonValue &value : bundles) {
        const QJsonObject bundle = value.toObject();
        const QString deviceId = bundle.value("deviceId").toString();
        const qint64 prekeyId = static_cast<qint64>(bundle.value("prekeyId").toDouble());
        const QByteArray identityPub =
            QByteArray::fromBase64(bundle.value("identityPub").toString().toLatin1());
        const QByteArray prekeyPub =
            QByteArray::fromBase64(bundle.value("prekeyPub").toString().toLatin1());
        if (deviceId.isEmpty() || prekeyId <= 0) {
            return {};
        }

        const auto eph = E2eeCrypto::generateX25519KeyPair();
        if (!eph.valid) {
            return {};
        }

        QByteArray shared = E2eeCrypto::ecdh(eph.privateKey, prekeyPub);
        shared += E2eeCrypto::ecdh(eph.privateKey, identityPub);
        SecureMemory::wipe(const_cast<QByteArray &>(eph.privateKey));
        if (shared.size() != 64) {
            SecureMemory::wipe(shared);
            return {};
        }

        QByteArray key = E2eeCrypto::deriveMessageKey(shared);
        SecureMemory::wipe(shared);
        if (key.isEmpty()) {
            return {};
        }

        const auto gcm = E2eeCrypto::aesGcmEncrypt(key, plainBytes);
        SecureMemory::wipe(key);
        if (!gcm.valid) {
            return {};
        }

        E2eeCrypto::EnvelopeEntry entry;
        entry.deviceId = deviceId;
        entry.prekeyId = prekeyId;
        entry.ephemeralPublicKey = eph.publicKey;
        entry.iv = gcm.iv;
        entry.ciphertext = gcm.ciphertext;
        entries.append(entry);
    }

    // 修复：追加发送方自己设备的拷贝（prekeyId=0，仅用本人身份密钥加密，
    // 不消费预密钥），使发送方重新登录/多端同步后仍能解密自己发出的消息
    if (m_identityKey.valid && !m_localDeviceId.isEmpty()) {
        const auto eph = E2eeCrypto::generateX25519KeyPair();
        if (eph.valid) {
            const QByteArray dh = E2eeCrypto::ecdh(eph.privateKey, m_identityKey.publicKey);
            SecureMemory::wipe(const_cast<QByteArray &>(eph.privateKey));
            if (dh.size() == 32) {
                QByteArray shared = dh + dh;
                SecureMemory::wipe(const_cast<QByteArray &>(dh));
                QByteArray key = E2eeCrypto::deriveMessageKey(shared);
                SecureMemory::wipe(shared);
                const auto gcm = E2eeCrypto::aesGcmEncrypt(key, plainBytes);
                SecureMemory::wipe(key);
                if (gcm.valid) {
                    E2eeCrypto::EnvelopeEntry selfEntry;
                    selfEntry.deviceId = m_localDeviceId;
                    selfEntry.prekeyId = E2eeCrypto::SelfCopyPrekeyId;
                    selfEntry.ephemeralPublicKey = eph.publicKey;
                    selfEntry.iv = gcm.iv;
                    selfEntry.ciphertext = gcm.ciphertext;
                    entries.append(selfEntry);
                }
            }
        }
    }

    if (entries.isEmpty()) {
        return {};
    }
    return QString::fromUtf8(
        QJsonDocument(E2eeCrypto::encodeEnvelope(entries)).toJson(QJsonDocument::Compact));
}

// 解密接收正文：非 envelope（M6 前存量明文）原样返回；否则逐本地预密钥尝试解密
QString NetworkManager::decryptIncomingContent(const QString &content, bool *undecryptable)
{
    using namespace XYChat::Security;

    if (undecryptable) {
        *undecryptable = false;
    }
    if (!E2eeCrypto::looksLikeEnvelope(content)) {
        return content; // 存量明文消息
    }

    bool ok = false;
    const auto entries = E2eeCrypto::decodeEnvelope(content, &ok);
    if (!ok) {
        if (undecryptable) {
            *undecryptable = true;
        }
        return {};
    }

    // 找到属于本设备的条目并尝试解密（同一台机器上收发双方 deviceId 可能
    // 相同，自身拷贝与接收方条目都要尝试，失败继续下一条）
    for (const auto &entry : entries) {
        if (entry.deviceId != m_localDeviceId) {
            continue;
        }
        if (!m_identityKey.valid) {
            continue;
        }

        if (entry.prekeyId == E2eeCrypto::SelfCopyPrekeyId) {
            // 自己设备的拷贝：仅用身份密钥解密，不消费预密钥
            QByteArray dh = E2eeCrypto::ecdh(m_identityKey.privateKey, entry.ephemeralPublicKey);
            if (dh.size() != 32) {
                continue;
            }
            QByteArray shared = dh + dh;
            SecureMemory::wipe(dh);
            QByteArray key = E2eeCrypto::deriveMessageKey(shared);
            SecureMemory::wipe(shared);
            const QByteArray plain = E2eeCrypto::aesGcmDecrypt(key, entry.iv, entry.ciphertext);
            SecureMemory::wipe(key);
            if (!plain.isEmpty()) {
                return QString::fromUtf8(plain);
            }
            continue;
        }

        // 服务端预密钥 ID 本地未知：逐个本地预密钥尝试，GCM 认证标签验证正确性
        for (int i = 0; i < m_localPrekeys.size(); ++i) {
            QByteArray shared = E2eeCrypto::ecdh(m_localPrekeys.at(i).privateKey,
                                                 entry.ephemeralPublicKey);
            shared += E2eeCrypto::ecdh(m_identityKey.privateKey, entry.ephemeralPublicKey);
            QByteArray key = E2eeCrypto::deriveMessageKey(shared);
            SecureMemory::wipe(shared);
            const QByteArray plain = E2eeCrypto::aesGcmDecrypt(key, entry.iv, entry.ciphertext);
            SecureMemory::wipe(key);
            if (!plain.isEmpty()) {
                // 一次性预密钥已消费：从本地删除（前向安全）；
                // 同消息的后续重复投递/重新同步由持久化解密缓存兜底
                m_localPrekeys.removeAt(i);
                KeyStorage::savePrekeys(m_username, m_localDeviceId, m_localPrekeys);
                return QString::fromUtf8(plain);
            }
        }
    }

    if (undecryptable) {
        *undecryptable = true;
    }
    return {};
}

// 在接收消息 JSON 上就地解密 content；失败时标记 undecryptable
void NetworkManager::decryptMessageObject(QJsonObject &msg)
{
    const qint64 msgId = msg.value("messageId").toVariant().toLongLong();

    // 同一消息可能经通知与同步重复投递：命中缓存避免重复消费预密钥
    if (msgId > 0 && m_decryptCache.contains(msgId)) {
        msg["content"] = m_decryptCache.value(msgId);
        return;
    }
    // M6.5: LocalStore 持久化解密缓存兜底（预密钥已消费后重新同步仍可恢复明文）
    if (msgId > 0) {
        const QString cached = m_localStore.loadDecryptedContent(msgId);
        if (!cached.isEmpty()) {
            msg["content"] = cached;
            m_decryptCache.insert(msgId, cached);
            return;
        }
    }

    const QString content = msg.value("content").toString();

    // M7b: 群聊 Sender Key 分发消息
    if (GroupE2eeCrypto::looksLikeDistribution(content)) {
        processGroupSenderKeyDistribution(msg);
        msg["content"] = "[Sender key updated]";
        msg["contentType"] = "system";
        return;
    }

    // M7b: 群聊 E2EE 消息
    if (GroupE2eeCrypto::looksLikeGroupMessage(content)) {
        decryptGroupMessageObject(msg);
        return;
    }

    bool undecryptable = false;
    const QString plain = decryptIncomingContent(content, &undecryptable);
    if (undecryptable) {
        // 防御统一：解密失败一律清空 content，绝不把 envelope 原文透传给 UI/落库
        msg["content"] = QString();
        msg["undecryptable"] = true;
    } else {
        msg["content"] = plain;
        if (msgId > 0) {
            m_decryptCache.insert(msgId, plain);
            if (m_decryptCache.size() > 2000) {
                m_decryptCache.clear();
            }
            // M6.5: 解密缓存归口 LocalStore（加密存储）；本地库不可用时回退
            // KeyStorage（DPAPI 保护），避免预密钥消费后明文不可恢复
            if (!m_localStore.saveDecryptedContent(msgId, plain)) {
                KeyStorage::saveDecryptCache(m_username, m_localDeviceId, m_decryptCache);
            }
        }
    }
}

// M9: 直接解密“编辑后”的新正文（绕过缓存、不预先清缓存）。
// 成功时同步持久化解密缓存并返回 true；失败返回 false。调用方在失败时回退
// 保留既有可读正文，绝不写空覆盖（否则重登后会把已可读消息破坏成“无法解密”）。
bool NetworkManager::decryptEditContent(const QJsonObject &data, QString &plaintext)
{
    const qint64 messageId = data.value("messageId").toVariant().toLongLong();
    const QString content = data.value("content").toString();

    if (GroupE2eeCrypto::looksLikeGroupMessage(content)) {
        // 群编辑正文：Sender-Key 密文，直接解密（内部会持久化缓存与 ratchet 状态）
        QJsonObject tmp = data;
        if (decryptGroupMessageObject(tmp)) {
            plaintext = tmp.value("content").toString();
            return !plaintext.isEmpty();
        }
        return false;
    }

    if (GroupE2eeCrypto::looksLikeDistribution(content)) {
        // 分发消息不可编辑，视为失败
        return false;
    }

    bool undecryptable = false;
    plaintext = decryptIncomingContent(content, &undecryptable);
    if (undecryptable || plaintext.isEmpty()) {
        return false;
    }
    // 1:1 正文：解密成功后同步内存缓存与持久化解密缓存（编辑前的旧明文缓存
    // 已被新明文覆盖，重登后按持久化缓存恢复）
    if (messageId > 0) {
        m_decryptCache.insert(messageId, plaintext);
        if (m_decryptCache.size() > 2000) {
            m_decryptCache.clear();
        }
        if (!m_localStore.saveDecryptedContent(messageId, plaintext)) {
            KeyStorage::saveDecryptCache(m_username, m_localDeviceId, m_decryptCache);
        }
    }
    return true;
}

// M7b: 确保本机在该群有 Sender Key；返回 true 表示 key 已可用
bool NetworkManager::ensureGroupSenderKey(qint64 conversationId,
                                          GroupE2eeCrypto::SenderKey &key)
{
    key = GroupE2eeCrypto::SenderKey{};
    if (conversationId <= 0 || m_userId <= 0 || m_localDeviceId.isEmpty()) {
        return false;
    }

    auto it = m_groupSenderKeys.find(conversationId);
    if (it != m_groupSenderKeys.end() && it->valid) {
        key = it.value();
        return true;
    }

    if (m_localStore.isOpen()) {
        const QString keyId = m_localStore.latestSenderKeyId(conversationId, m_userId, m_localDeviceId);
        if (!keyId.isEmpty()) {
            QByteArray chainKey;
            QByteArray publicSigningKey;
            QByteArray privateSigningKey;
            int iteration = 0;
            if (m_localStore.loadSenderKey(conversationId, m_userId, m_localDeviceId,
                                           keyId, chainKey, publicSigningKey,
                                           privateSigningKey, iteration)) {
                GroupE2eeCrypto::SenderKey stored;
                stored.keyId = keyId;
                stored.chainKey = chainKey;
                stored.publicSigningKey = publicSigningKey;
                stored.privateSigningKey = privateSigningKey;
                stored.iteration = iteration;
                stored.valid = !keyId.isEmpty() && chainKey.size() == 32
                    && publicSigningKey.size() == 32 && privateSigningKey.size() == 32;
                if (stored.valid) {
                    m_groupSenderKeys.insert(conversationId, stored);
                    key = stored;
                    return true;
                }
            }
        }
    }
    key = GroupE2eeCrypto::SenderKey{};
    return false;
}

void NetworkManager::sendFetchGroupKeysRequest(qint64 conversationId)
{
    if (m_state != ConnectionState::Authenticated || conversationId <= 0) {
        return;
    }
    QJsonObject json;
    json["type"] = "fetch_group_keys";
    json["conversationId"] = conversationId;
    addReplayProtection(json);

    Packet packet;
    packet.messageType = MessageType::FetchGroupKeysRequest;
    packet.requestId = nextRequestId();
    packet.payload = QJsonDocument(json).toJson(QJsonDocument::Compact);
    m_pendingFetchGroupKeysRequestId = packet.requestId;
    m_fetchGroupKeysTargetConvId = conversationId;
    sendPacket(packet);
}

void NetworkManager::handleFetchGroupKeysResponse(const Packet &packet)
{
    if (packet.requestId != m_pendingFetchGroupKeysRequestId) {
        return;
    }
    const qint64 convId = m_fetchGroupKeysTargetConvId;
    m_pendingFetchGroupKeysRequestId = 0;
    m_fetchGroupKeysTargetConvId = 0;

    const QJsonObject response = QJsonDocument::fromJson(packet.payload).object();
    const int code = response.value("code").toInt();
    if (code != static_cast<int>(ErrorCode::Ok) || convId <= 0) {
        qWarning() << "[NetMgr] FetchGroupKeys failed:" << response.value("message").toString();
        // P1-3: 瞬时失败（限流/内部错误/超时）延迟重试轮换，避免后向安全窗口；
        // 确定性失败（越权/会话不存在）丢弃，待下次成员变更或发送再触发。
        // M11: 服务端限流错误码由 LoginRateLimited 迁至通用 RateLimited，两者均视为瞬时。
        const bool transient = code == static_cast<int>(ErrorCode::RateLimited)
            || code == static_cast<int>(ErrorCode::LoginRateLimited)
            || code == static_cast<int>(ErrorCode::InternalError)
            || code == static_cast<int>(ErrorCode::Timeout);
        if (transient && convId > 0) {
            m_healQueue.insert(convId);
            QTimer::singleShot(5000, this, [this]() { drainHealQueue(); });
        } else {
            drainHealQueue();
        }
        return;
    }

    const QJsonObject data = response.value("data").toObject();
    const QJsonObject bundlesByUser = data.value("bundles").toObject();
    // 诊断：服务端返回的成员密钥包数量（少于群成员数说明有人被 fetch_group_keys 跳过）
    qInfo() << "[NetMgr] FetchGroupKeys for conv" << convId
            << "returned member bundles:" << bundlesByUser.size();
    const QString distribution = buildGroupSenderKeyDistribution(convId, bundlesByUser);
    if (!distribution.isEmpty()) {
        const QString clientMessageId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QJsonObject json;
        json["type"] = "send_message";
        json["conversationId"] = convId;
        json["content"] = distribution;
        json["contentType"] = "sender_key_distribution";
        json["clientMessageId"] = clientMessageId;
        addReplayProtection(json);

        Packet distPacket;
        distPacket.messageType = MessageType::SendMessageRequest;
        distPacket.requestId = nextRequestId();
        distPacket.payload = QJsonDocument(json).toJson(QJsonDocument::Compact);
        m_pendingSendByRequestId.insert(distPacket.requestId, clientMessageId);
        m_pendingGroupDistributions.insert(clientMessageId);
        sendPacket(distPacket);
    } else {
        qWarning() << "[NetMgr] Failed to build group sender key distribution for" << convId;
    }

    // P1-3: 推进 healing 队列（逐群轮换，单发槽位约束下每次一个）
    drainHealQueue();
}

QString NetworkManager::buildGroupSenderKeyDistribution(qint64 conversationId,
                                                        const QJsonObject &bundlesByUser)
{
    if (conversationId <= 0 || bundlesByUser.isEmpty()) {
        return {};
    }

    GroupE2eeCrypto::SenderKey key = GroupE2eeCrypto::generateSenderKey();
    if (!key.valid) {
        return {};
    }

    QList<GroupE2eeCrypto::DistributionEntry> entries;
    const QString chainKeyB64 = QString::fromLatin1(key.chainKey.toBase64());
    int coveredUsers = 0;
    QList<qint64> skippedUsers;

    for (auto it = bundlesByUser.constBegin(); it != bundlesByUser.constEnd(); ++it) {
        const qint64 userId = it.key().toLongLong();
        if (userId <= 0) {
            continue;
        }
        const QJsonArray bundles = it.value().toArray();
        if (bundles.isEmpty()) {
            // 该成员无密钥包：未注册 E2EE 或预密钥耗尽被服务端 fetch_group_keys 跳过
            skippedUsers.append(userId);
            continue;
        }
        // 复用 pairwise E2EE 加密 chainKey，然后解析出各设备条目
        const QString envelope = encryptForUser(userId, bundles, chainKeyB64);
        if (envelope.isEmpty()) {
            skippedUsers.append(userId);
            continue;
        }
        bool ok = false;
        const auto deviceEntries = E2eeCrypto::decodeEnvelope(envelope, &ok);
        if (!ok) {
            skippedUsers.append(userId);
            continue;
        }
        ++coveredUsers;
        for (const auto &entry : deviceEntries) {
            GroupE2eeCrypto::DistributionEntry distEntry;
            distEntry.userId = userId;
            distEntry.deviceId = entry.deviceId;
            distEntry.envelope = entry;
            entries.append(distEntry);
        }
    }

    // 诊断：暴露分发覆盖了哪些成员、跳过了哪些（跳过者将收不到 chain key → 群消息不可解）
    qInfo() << "[NetMgr] Built group sender key distribution for conv" << conversationId
            << "coveredUsers" << coveredUsers << "skippedUsers" << skippedUsers;

    if (entries.isEmpty()) {
        SecureMemory::wipe(key.chainKey);
        SecureMemory::wipe(key.privateSigningKey);
        return {};
    }

    // 保存发送方 Sender Key（chainKey 与私钥经 LocalStore 存储密钥加密）
    if (m_localStore.isOpen()) {
        m_localStore.saveSenderKey(conversationId, m_userId, m_localDeviceId,
                                   key.keyId, key.chainKey,
                                   key.publicSigningKey, key.privateSigningKey,
                                   key.iteration);
    }
    m_groupSenderKeys.insert(conversationId, key);

    const QJsonObject distJson = GroupE2eeCrypto::encodeDistribution(
        conversationId, m_userId, m_localDeviceId, key, entries);
    return QString::fromUtf8(QJsonDocument(distJson).toJson(QJsonDocument::Compact));
}

// 解密单个 pairwise envelope 条目（用于提取 group Sender Key 的 chainKey）
static QByteArray decryptEnvelopeEntry(const XYChat::Security::E2eeCrypto::EnvelopeEntry &entry,
                                       const XYChat::Security::E2eeCrypto::KeyPair &identityKey,
                                       QList<KeyStorage::PrekeyEntry> &localPrekeys,
                                       const QString &username,
                                       const QString &deviceId)
{
    using namespace XYChat::Security;
    if (!identityKey.valid || entry.deviceId != deviceId) {
        return {};
    }

    if (entry.prekeyId == E2eeCrypto::SelfCopyPrekeyId) {
        QByteArray dh = E2eeCrypto::ecdh(identityKey.privateKey, entry.ephemeralPublicKey);
        if (dh.size() != 32) {
            return {};
        }
        QByteArray shared = dh + dh;
        SecureMemory::wipe(dh);
        QByteArray key = E2eeCrypto::deriveMessageKey(shared);
        SecureMemory::wipe(shared);
        const QByteArray plain = E2eeCrypto::aesGcmDecrypt(key, entry.iv, entry.ciphertext);
        SecureMemory::wipe(key);
        return plain;
    }

    for (int i = 0; i < localPrekeys.size(); ++i) {
        QByteArray shared = E2eeCrypto::ecdh(localPrekeys.at(i).privateKey,
                                             entry.ephemeralPublicKey);
        shared += E2eeCrypto::ecdh(identityKey.privateKey, entry.ephemeralPublicKey);
        QByteArray key = E2eeCrypto::deriveMessageKey(shared);
        SecureMemory::wipe(shared);
        const QByteArray plain = E2eeCrypto::aesGcmDecrypt(key, entry.iv, entry.ciphertext);
        SecureMemory::wipe(key);
        if (!plain.isEmpty()) {
            localPrekeys.removeAt(i);
            KeyStorage::savePrekeys(username, deviceId, localPrekeys);
            return plain;
        }
    }
    return {};
}

bool NetworkManager::processGroupSenderKeyDistribution(const QJsonObject &msg)
{
    const qint64 convId = msg.value("conversationId").toVariant().toLongLong();
    const QString content = msg.value("content").toString();
    if (convId <= 0 || content.isEmpty()) {
        return false;
    }

    qint64 groupId = 0;
    qint64 senderUserId = 0;
    QString senderDeviceId;
    GroupE2eeCrypto::SenderKey key;
    QList<GroupE2eeCrypto::DistributionEntry> entries;
    if (!GroupE2eeCrypto::decodeDistribution(content, groupId, senderUserId,
                                             senderDeviceId, key, entries)) {
        return false;
    }

    if (groupId != convId || senderUserId <= 0 || senderDeviceId.isEmpty()) {
        return false;
    }

    QByteArray decryptedChainKeyB64;
    bool foundSelfEntry = false;
    // 修复：遍历所有属于本机的条目逐个尝试（与单聊 decryptIncomingContent 一致），
    // 不因第一个条目失败就放弃——本机可能同时有预密钥条目与自身拷贝条目
    // （prekeyId=0，仅身份密钥加密），后者在预密钥已消费时仍可稳定解出
    for (const auto &entry : entries) {
        if (entry.userId == m_userId && entry.deviceId == m_localDeviceId) {
            foundSelfEntry = true;
            decryptedChainKeyB64 = decryptEnvelopeEntry(entry.envelope, m_identityKey, m_localPrekeys,
                                                        m_username, m_localDeviceId);
            if (!decryptedChainKeyB64.isEmpty()) {
                break;
            }
        }
    }
    if (decryptedChainKeyB64.isEmpty()) {
        // 诊断：区分“分发中无本机条目”（发送方 fetch_group_keys 未覆盖本机/预密钥耗尽）
        // 与“有条目但解密失败”（本机 E2EE 私钥/预密钥不匹配）
        qWarning() << "[NetMgr] Group sender key distribution unusable for group" << convId
                   << "sender" << senderUserId << "selfEntryPresent" << foundSelfEntry
                   << "entries" << entries.size() << "identityKeyValid" << m_identityKey.valid
                   << "localPrekeys" << m_localPrekeys.size();
        return false;
    }

    const QByteArray chainKey = QByteArray::fromBase64(decryptedChainKeyB64,
                                                       QByteArray::AbortOnBase64DecodingErrors);
    if (chainKey.size() != 32) {
        qWarning() << "[NetMgr] Decoded sender key has wrong length" << chainKey.size()
                   << "for group" << convId;
        return false;
    }

    if (m_localStore.isOpen()) {
        m_localStore.saveSenderKey(convId, senderUserId, senderDeviceId, key.keyId,
                                   chainKey, key.publicSigningKey,
                                   QByteArray(), key.iteration);
    }
    qInfo() << "[NetMgr] Installed group sender key for group" << convId
            << "from user" << senderUserId << "keyId" << key.keyId;
    return true;
}

QString NetworkManager::encryptGroupMessage(qint64 conversationId, const QString &plaintext)
{
    GroupE2eeCrypto::SenderKey key;
    if (!ensureGroupSenderKey(conversationId, key) || !key.valid) {
        return {};
    }

    // 从内存缓存中取出可修改的副本（ratchet 会修改 chainKey/iteration）
    GroupE2eeCrypto::SenderKey mutableKey = m_groupSenderKeys.value(conversationId);
    if (!mutableKey.valid) {
        return {};
    }

    const auto encrypted = GroupE2eeCrypto::encryptMessage(mutableKey, plaintext.toUtf8());
    if (!encrypted.valid) {
        return {};
    }

    // 保存 ratchet 后的状态
    if (m_localStore.isOpen()) {
        m_localStore.saveSenderKey(conversationId, m_userId, m_localDeviceId,
                                   mutableKey.keyId, mutableKey.chainKey,
                                   mutableKey.publicSigningKey,
                                   mutableKey.privateSigningKey, mutableKey.iteration);
    }
    m_groupSenderKeys.insert(conversationId, mutableKey);

    const QJsonObject envelope = GroupE2eeCrypto::encodeGroupMessage(encrypted, m_localDeviceId);
    return QString::fromUtf8(QJsonDocument(envelope).toJson(QJsonDocument::Compact));
}

bool NetworkManager::decryptGroupMessageObject(QJsonObject &msg)
{
    const qint64 msgId = msg.value("messageId").toVariant().toLongLong();
    const qint64 convId = msg.value("conversationId").toVariant().toLongLong();
    const QString content = msg.value("content").toString();

    GroupE2eeCrypto::EncryptedMessage encrypted;
    QString senderDeviceId;
    if (!GroupE2eeCrypto::decodeGroupMessage(content, encrypted, &senderDeviceId)) {
        // 修复：decode 失败也必须清空 content 并标记 undecryptable，
        // 否则群 envelope 原文会透传到 UI/落库（密文当正文显示）
        qWarning() << "[NetMgr] Group envelope decode failed for conversation" << convId
                   << "sender" << msg.value("senderId").toVariant().toLongLong()
                   << "contentLength" << content.size();
        msg["content"] = QString();
        msg["undecryptable"] = true;
        return false;
    }

    qint64 senderUserId = msg.value("senderId").toVariant().toLongLong();

    QByteArray chainKey;
    QByteArray publicSigningKey;
    QByteArray privateSigningKey;
    int iteration = 0;
    bool loaded = false;
    if (m_localStore.isOpen()) {
        // M9 修复：历史 message_edited 事件与推送 payload 不带 senderId，而 Sender Key
        // 以（群, 发送者, 设备, keyId）定位；此处按设备 + keyId 反查，避免群消息
        // 编辑后接收端因寻址失败而把已可读的正文清成“无法解密”
        if (senderUserId <= 0) {
            senderUserId = m_localStore.senderUserIdForKey(convId, senderDeviceId,
                                                           encrypted.keyId);
            if (senderUserId > 0) {
                msg["senderId"] = senderUserId;
            }
        }
        loaded = m_localStore.loadSenderKey(convId, senderUserId, senderDeviceId,
                                            encrypted.keyId, chainKey, publicSigningKey,
                                            privateSigningKey, iteration);
    }
    if (!loaded) {
        qWarning() << "[NetMgr] No sender key for group" << convId
                   << "sender" << senderUserId << "device" << senderDeviceId
                   << "keyId" << encrypted.keyId;
        msg["content"] = QString();
        msg["undecryptable"] = true;
        return false;
    }

    // M9 修复：载入已跳过的消息密钥缓存。编辑会以更大的 iteration 覆盖旧消息正文，
    // 使 iteration 与 message_id 顺序解耦；按消息 id 升序补收（sync_messages）时，
    // 先解到高 iteration 会使后到的低 iteration 消息被回滚检查永久拒绝
    QMap<int, QByteArray> skippedKeys;
    if (m_localStore.isOpen()) {
        skippedKeys = m_localStore.loadSkippedMessageKeys(convId, senderUserId,
                                                          senderDeviceId, encrypted.keyId);
    }
    const bool hadSkippedKeys = !skippedKeys.isEmpty();

    const QByteArray plain = GroupE2eeCrypto::decryptMessage(chainKey, iteration,
                                                             publicSigningKey, encrypted,
                                                             &skippedKeys);
    if (plain.isEmpty()) {
        qWarning() << "[NetMgr] Group message decryption failed for group" << convId
                   << "sender" << senderUserId << "keyId" << encrypted.keyId
                   << "iteration" << encrypted.iteration;
        msg["content"] = QString();
        msg["undecryptable"] = true;
        return false;
    }

    msg["content"] = QString::fromUtf8(plain);
    if (msgId > 0) {
        m_decryptCache.insert(msgId, QString::fromUtf8(plain));
        if (m_decryptCache.size() > 2000) {
            m_decryptCache.clear();
        }
        if (!m_localStore.saveDecryptedContent(msgId, QString::fromUtf8(plain))) {
            KeyStorage::saveDecryptCache(m_username, m_localDeviceId, m_decryptCache);
        }
    }

    // 保存 ratchet 后的 chainKey/iteration
    m_localStore.saveSenderKey(convId, senderUserId, senderDeviceId, encrypted.keyId,
                               chainKey, publicSigningKey, QByteArray(), iteration);
    // 仅在缓存确有变化时落库（新增跳序密钥，或消费/清空了原有缓存）
    if (m_localStore.isOpen() && (!skippedKeys.isEmpty() || hadSkippedKeys)) {
        m_localStore.saveSkippedMessageKeys(convId, senderUserId, senderDeviceId,
                                            encrypted.keyId, skippedKeys);
    }
    return true;
}

// P1-3: 触发指定群的 Sender-Key 轮换与重分发。
// 复用 fetch_group_keys → buildGroupSenderKeyDistribution（内部 generateSenderKey 生成新 keyId）
// → 以 sender_key_distribution 群消息分发；pairwise 条目仅现任成员可解，
// 从而新成员获得当前密钥、被移除成员因密钥轮换失去后续消息的解密能力（后向安全）。
void NetworkManager::healGroupSenderKey(qint64 conversationId)
{
    if (conversationId <= 0) {
        return;
    }
    // 去重：同一群已在途或已排队时跳过（避免成员变更风暴放大预密钥消耗）
    if (m_fetchGroupKeysTargetConvId == conversationId || m_healQueue.contains(conversationId)) {
        return;
    }
    // 未认证/E2EE 未就绪/单发槽位被占用：入队待推进（不丢弃，保证离线补偿与登录后补触发）
    if (m_state != ConnectionState::Authenticated || !m_e2eeReady
        || m_pendingFetchGroupKeysRequestId != 0) {
        m_healQueue.insert(conversationId);
        return;
    }
    sendFetchGroupKeysRequest(conversationId);
}

// P1-3: 单发槽位空闲且已就绪时，从队列取出一个群推进轮换重分发（每次一个，
// 其响应回到 handleFetchGroupKeysResponse 后继续推进，天然受 fetch 限流节流）
void NetworkManager::drainHealQueue()
{
    if (m_state != ConnectionState::Authenticated || !m_e2eeReady
        || m_pendingFetchGroupKeysRequestId != 0 || m_healQueue.isEmpty()) {
        return;
    }
    auto next = m_healQueue.constBegin();
    const qint64 nextConv = *next;
    m_healQueue.erase(next);
    sendFetchGroupKeysRequest(nextConv);
}

// P1: 服务端 MessageType::Error 回包处理（鉴权门 validateSession 失败、校验拒绝等）。
// 关键作用：清理在途单发槽位，避免 Error 回包（非对应响应类型）导致
// m_pendingFetchGroupKeysRequestId 永久占用 → healing 队列与 outbox 群分支卡死。
void NetworkManager::handleErrorResponse(const Packet &packet)
{
    const QJsonObject response = QJsonDocument::fromJson(packet.payload).object();
    const int code = response.value("code").toInt();
    qWarning() << "[NetMgr] Server error response, code" << code
               << "message" << response.value("message").toString();

    // P2: 会话已失效（过期/被终止）——任何业务请求被鉴权门拒绝即触发，
    // 清状态并回登录页，避免静默卡死
    if (code == static_cast<int>(ErrorCode::SessionInvalid)
        || code == static_cast<int>(ErrorCode::SessionExpired)) {
        notifySessionExpired();
        return;
    }

    if (packet.requestId != 0 && packet.requestId == m_pendingFetchGroupKeysRequestId) {
        m_pendingFetchGroupKeysRequestId = 0;
        m_fetchGroupKeysTargetConvId = 0;
        drainHealQueue();
    }
    if (packet.requestId != 0 && packet.requestId == m_pendingFetchKeysRequestId) {
        m_pendingFetchKeysRequestId = 0;
        m_fetchKeysTargetUserId = 0;
    }
    if (packet.requestId != 0 && packet.requestId == m_pendingRegisterKeysRequestId) {
        m_pendingRegisterKeysRequestId = 0;
        m_e2eeBootstrapPending = false;
    }
}

// M3: 确认消息
void NetworkManager::ackMessage(qint64 messageId, const QString &status)
{
    if (m_state != ConnectionState::Authenticated) return;

    QJsonObject json;
    json["type"] = "ack_message";
    json["messageId"] = messageId;
    json["status"] = status;
    addReplayProtection(json);

    Packet packet;
    packet.messageType = MessageType::AckMessageRequest;
    packet.requestId = nextRequestId();
    packet.payload = QJsonDocument(json).toJson(QJsonDocument::Compact);
    m_pendingAckMessageRequestId = packet.requestId;
    sendPacket(packet);
}

// M3: 同步消息
void NetworkManager::syncMessages(qint64 conversationId, qint64 afterId, int limit)
{
    // M6.5: 首页拉取先立即展示本地缓存（重启后即刻可见、离线可查），
    // 随后服务端响应到达时以权威数据覆盖
    if (afterId == 0 && conversationId > 0) {
        const QJsonArray cached = m_localStore.loadMessages(conversationId, limit);
        if (!cached.isEmpty()) {
            // M8.2: 本地缓存表无 file_id 列，回填的消息正文可能是清单（含密钥），
            // 同样必经脱敏出口（attachFileInfo 会从清单里取回 fileId 并登记）
            emit messagesSynced(conversationId, sanitizeArrayForUi(cached), false);
        }
    }

    if (m_state != ConnectionState::Authenticated) return;

    QJsonObject json;
    json["type"] = "sync_messages";
    json["conversationId"] = conversationId;
    json["afterId"] = afterId;
    json["limit"] = limit;
    addReplayProtection(json);

    Packet packet;
    packet.messageType = MessageType::SyncMessagesRequest;
    packet.requestId = nextRequestId();
    packet.payload = QJsonDocument(json).toJson(QJsonDocument::Compact);
    m_pendingSyncMessagesRequestId = packet.requestId;
    sendPacket(packet);
}

// M3: 响应处理
void NetworkManager::handleSearchUsersResponse(const Packet &packet)
{
    if (packet.requestId != m_pendingSearchRequestId) return;
    m_pendingSearchRequestId = 0;

    const QJsonObject response = QJsonDocument::fromJson(packet.payload).object();
    if (response.value("code").toInt() == static_cast<int>(ErrorCode::Ok)) {
        emit searchUsersResult(response.value("data").toObject().value("users").toArray());
    }
}

void NetworkManager::handleAddContactResponse(const Packet &packet)
{
    if (packet.requestId != m_pendingAddContactRequestId) return;
    m_pendingAddContactRequestId = 0;

    const QJsonObject response = QJsonDocument::fromJson(packet.payload).object();
    if (response.value("code").toInt() != static_cast<int>(ErrorCode::Ok)) {
        emit messageSendFailed(response.value("message").toString("Failed to add contact"));
    }
}

void NetworkManager::handleGetContactsResponse(const Packet &packet)
{
    if (packet.requestId != m_pendingGetContactsRequestId) return;
    m_pendingGetContactsRequestId = 0;

    const QJsonObject response = QJsonDocument::fromJson(packet.payload).object();
    if (response.value("code").toInt() == static_cast<int>(ErrorCode::Ok)) {
        emit contactsResult(response.value("data").toObject().value("contacts").toArray());
    }
}

void NetworkManager::handleGetConversationsResponse(const Packet &packet)
{
    if (packet.requestId != m_pendingGetConversationsRequestId) return;
    m_pendingGetConversationsRequestId = 0;

    const QJsonObject response = QJsonDocument::fromJson(packet.payload).object();
    if (response.value("code").toInt() == static_cast<int>(ErrorCode::Ok)) {
        // M6: 会话预览中的 envelope 密文替换为占位文本
        QJsonArray conversations =
            response.value("data").toObject().value("conversations").toArray();
        for (QJsonValueRef value : conversations) {
            QJsonObject conv = value.toObject();
            const QString lastMessage = conv.value("lastMessage").toString();
            // 密文预览一律替换为占位：私聊 pairwise envelope + 群 e2ee_group +
            // sender_key_distribution。群 envelope 是 JSON（type=group_e2ee），
            // 此前只识别 pairwise envelope，导致群密文 JSON 被当作正文直接展示
            if (XYChat::Security::E2eeCrypto::looksLikeEnvelope(lastMessage)
                || GroupE2eeCrypto::looksLikeGroupMessage(lastMessage)
                || GroupE2eeCrypto::looksLikeDistribution(lastMessage)) {
                conv["lastMessage"] = "[Encrypted message]";
                value = conv;
                continue;
            }
            // M7a: 群系统消息预览（结构化 JSON）转为可读摘要
            const QJsonObject sysObj = QJsonDocument::fromJson(lastMessage.toUtf8()).object();
            if (!sysObj.isEmpty() && sysObj.contains("event")) {
                conv["lastMessage"] = systemMessageSummary(lastMessage);
                value = conv;
            }
        }
        // M6.5: 服务端权威会话数据写入本地缓存；预览为占位符时先用本地
        // 解密缓存回填真实明文，避免持久化预览退化为 "[Encrypted message]"
        for (QJsonValueRef value : conversations) {
            QJsonObject conv = value.toObject();
            if (conv.value("lastMessage").toString() == "[Encrypted message]") {
                const qint64 lastMessageId =
                    conv.value("lastMessageId").toVariant().toLongLong();
                const QString cached = m_localStore.loadDecryptedContent(lastMessageId);
                if (!cached.isEmpty()) {
                    conv["lastMessage"] = cached;
                    value = conv;
                }
            }
            m_localStore.upsertConversation(conv);
        }
        emit conversationsResult(conversations);
    }
}

void NetworkManager::handleSendMessageResponse(const Packet &packet)
{
    auto it = m_pendingSendByRequestId.constFind(packet.requestId);
    if (it == m_pendingSendByRequestId.constEnd()) return;
    const QString clientMessageId = it.value();
    m_pendingSendByRequestId.erase(it);

    const QJsonObject response = QJsonDocument::fromJson(packet.payload).object();
    const int code = response.value("code").toInt();
    if (code == static_cast<int>(ErrorCode::Ok)) {
        const QJsonObject data = response.value("data").toObject();
        // 确认后从 outbox 移除（以服务端回传的幂等键为准）
        const QString ackedId = data.value("clientMessageId").toString(clientMessageId);

        // M7b: 群聊 Sender Key 分发消息 ACK：不展示、不落库，触发后续群消息发送
        if (m_pendingGroupDistributions.remove(ackedId)) {
            for (int i = m_outbox.size() - 1; i >= 0; --i) {
                if (m_outbox.at(i).clientMessageId == ackedId) {
                    m_localStore.removeOutboxItem(ackedId);
                    m_outbox.removeAt(i);
                    break;
                }
            }
            flushOutbox();
            return;
        }

        QString sentContent;
        for (int i = 0; i < m_outbox.size(); ++i) {
            if (m_outbox.at(i).clientMessageId == ackedId) {
                sentContent = m_outbox.at(i).content;
                m_outbox.removeAt(i);
                break;
            }
        }
        // M6.5: 同步移除持久化 outbox，并将已发送消息写入本地缓存
        m_localStore.removeOutboxItem(ackedId);
        const qint64 messageId = data.value("messageId").toVariant().toLongLong();
        const qint64 conversationId = data.value("conversationId").toVariant().toLongLong();
        if (messageId > 0 && conversationId > 0 && !sentContent.isEmpty()) {
            QJsonObject cached;
            cached["messageId"] = messageId;
            cached["conversationId"] = conversationId;
            cached["senderId"] = m_userId;
            cached["senderUsername"] = m_username;
            cached["content"] = sentContent;
            cached["contentType"] = "text";
            cached["status"] = "sent";
            cached["clientMessageId"] = ackedId;
            cached["createdAt"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
            // M8.2: 带上 messageId，本地缓存回填时才能凭它把清单登记到传输
            // 引擎（否则发送方自己也无法下载/另存刚发的文件）
            cached["messageId"] = messageId;
            m_localStore.upsertMessage(cached);
        }
        emit messageSent(messageId, conversationId, ackedId);
    } else {
        // M7a: 群消息的确定性失败（已不在群/会话不存在/请求非法）移除
        // outbox 项避免无限重试；瞬时错误保留重试
        const int code2 = response.value("code").toInt();
        const bool deterministic = code2 == static_cast<int>(ErrorCode::PermissionDenied)
            || code2 == static_cast<int>(ErrorCode::ConversationNotFound)
            || code2 == static_cast<int>(ErrorCode::InvalidRequest);
        if (deterministic) {
            for (int i = m_outbox.size() - 1; i >= 0; --i) {
                if (m_outbox.at(i).clientMessageId == clientMessageId) {
                    if (m_outbox.at(i).conversationId > 0) {
                        m_localStore.removeOutboxItem(clientMessageId);
                        m_outbox.removeAt(i);
                    }
                    break;
                }
            }
        } else if (!m_outboxFlushScheduled) {
            // M11: 瞬时失败（限流 RateLimited/内部错误/超时）保留 outbox，短退避后自动重刷。
            // 群消息直发不经 fetch_keys 链路，若不调度重刷，超限消息会滞留至下次登录；
            // 单发护栏避免一次突发（多条被限流）堆叠多个定时器。
            m_outboxFlushScheduled = true;
            QTimer::singleShot(3000, this, [this]() {
                m_outboxFlushScheduled = false;
                flushOutbox();
            });
        }
        emit messageSendFailed(response.value("message").toString("Send failed"));
    }
}

void NetworkManager::handleAckMessageResponse(const Packet &packet)
{
    if (packet.requestId != m_pendingAckMessageRequestId) return;
    m_pendingAckMessageRequestId = 0;

    const QJsonObject response = QJsonDocument::fromJson(packet.payload).object();
    if (response.value("code").toInt() == static_cast<int>(ErrorCode::Ok)) {
        emit messageAcked(response.value("data").toObject().value("messageId").toVariant().toLongLong());
    }
}

void NetworkManager::handleSyncMessagesResponse(const Packet &packet)
{
    if (packet.requestId != m_pendingSyncMessagesRequestId) return;
    m_pendingSyncMessagesRequestId = 0;

    const QJsonObject response = QJsonDocument::fromJson(packet.payload).object();
    if (response.value("code").toInt() == static_cast<int>(ErrorCode::Ok)) {
        const QJsonObject data = response.value("data").toObject();
        // M6: 逐条解密同步到的消息正文
        QJsonArray messages = data.value("messages").toArray();
        QJsonArray visibleMessages;
        for (QJsonValueRef value : messages) {
            QJsonObject msg = value.toObject();
            const QString ct = msg.value("contentType").toString();
            const QString c = msg.value("content").toString();
            // M7b: 群聊 Sender Key 分发消息只处理、不展示、不落库
            if (ct == QLatin1String("sender_key_distribution")
                || GroupE2eeCrypto::looksLikeDistribution(c)) {
                processGroupSenderKeyDistribution(msg);
                continue;
            }
            decryptMessageObject(msg);
            // M6.5: 写入本地缓存（加密存储，保留清单原文以便重启后恢复）
            m_localStore.upsertMessage(msg);
            // M8.2: 落库之后再脱敏：交给 UI 的消息不得携带清单（含文件密钥）
            sanitizeForUi(msg);
            visibleMessages.append(msg);
        }
        // 修复：emit 解密后的 visibleMessages（原始 messages 的 content 为 envelope
        // 密文且无 undecryptable 标记，直接渲染会把密文当正文显示——私聊离线/群聊历史）
        // 边界保护：本页全为不可见消息（如密钥分发）时跳过 emit，
        // 避免用空列表覆盖此前由本地缓存填充的聊天视图
        if (messages.isEmpty() || !visibleMessages.isEmpty()) {
            emit messagesSynced(
                data.value("conversationId").toVariant().toLongLong(),
                visibleMessages,
                data.value("hasMore").toBool());
        }
    }
}

// M8.2: 文件传输引擎（供 main.cpp 注册为 QML context property）
QObject *NetworkManager::fileTransfer() const
{
    return m_fileTransfer;
}

// M8.2: 把传输引擎的控制面请求接到 TCP 通道。引擎不持有 socket，
// 只发信号并等回调，因此可脱离网络单测
void NetworkManager::wireFileTransfer()
{
    using FT = XYChat::Client::FileTransferManager;

    connect(m_fileTransfer, &FT::uploadCreateRequested, this,
            [this](qint64 seq, qint64 cipherSize, qint64 chunkSize, int chunkCount,
                   const QString &sha256Hex) {
                QJsonObject fields;
                fields["type"] = "file_upload_create";
                fields["sizeBytes"] = cipherSize;
                fields["chunkSize"] = chunkSize;
                fields["chunkCount"] = chunkCount;
                fields["sha256"] = sha256Hex;
                sendFileControlRequest(MessageType::FileUploadCreateRequest, seq, fields);
            });
    connect(m_fileTransfer, &FT::uploadQueryRequested, this,
            [this](qint64 seq, qint64 fileId) {
                QJsonObject fields;
                fields["type"] = "file_upload_query";
                fields["fileId"] = fileId;
                sendFileControlRequest(MessageType::FileUploadQueryRequest, seq, fields);
            });
    connect(m_fileTransfer, &FT::uploadCompleteRequested, this,
            [this](qint64 seq, qint64 fileId) {
                QJsonObject fields;
                fields["type"] = "file_upload_complete";
                fields["fileId"] = fileId;
                sendFileControlRequest(MessageType::FileUploadCompleteRequest, seq, fields);
            });
    connect(m_fileTransfer, &FT::uploadCancelRequested, this,
            [this](qint64 seq, qint64 fileId) {
                QJsonObject fields;
                fields["type"] = "file_upload_cancel";
                fields["fileId"] = fileId;
                sendFileControlRequest(MessageType::FileUploadCancelRequest, seq, fields);
            });
    connect(m_fileTransfer, &FT::downloadTicketRequested, this,
            [this](qint64 seq, qint64 fileId) {
                QJsonObject fields;
                fields["type"] = "file_download_ticket";
                fields["fileId"] = fileId;
                sendFileControlRequest(MessageType::FileDownloadTicketRequest, seq, fields);
            });

    // 上传完成：把清单作为消息正文经既有 E2EE（私聊 envelope / 群 Sender-Key）
    // 加密后发送，并带上 fileId。清单明文只在本进程 C++ 侧流转，不进 QML
    connect(m_fileTransfer, &FT::sendMessageRequested, this,
            [this](qint64 conversationId, qint64 peerUserId, const QString &manifestJson,
                   qint64 fileId) {
                if (conversationId > 0) {
                    sendGroupMessage(conversationId, manifestJson, fileId);
                } else if (peerUserId > 0) {
                    sendMessage(peerUserId, manifestJson, fileId);
                } else {
                    emit messageSendFailed("Invalid file message target");
                }
            });
}

void NetworkManager::sendFileControlRequest(MessageType type, qint64 seq,
                                            const QJsonObject &fields)
{
    if (m_state != ConnectionState::Authenticated) {
        // 未认证时不能发控制面请求：直接把失败回给引擎，让它上报而不是静默挂起
        switch (type) {
        case MessageType::FileUploadCreateRequest:
            m_fileTransfer->onUploadCreated(seq, false, 0, QString(), "Not authenticated");
            return;
        case MessageType::FileUploadQueryRequest:
            m_fileTransfer->onUploadQueried(seq, false, QList<int>(), "Not authenticated");
            return;
        case MessageType::FileUploadCompleteRequest:
            m_fileTransfer->onUploadCompleted(seq, false, "Not authenticated");
            return;
        case MessageType::FileUploadCancelRequest:
            m_fileTransfer->onUploadCancelled(seq, false, "Not authenticated");
            return;
        default:
            m_fileTransfer->onDownloadTicket(seq, false, QString(), 0, 0, 0, QString(),
                                             "Not authenticated");
            return;
        }
    }

    QJsonObject json = fields;
    addReplayProtection(json);

    Packet packet;
    packet.messageType = type;
    packet.requestId = nextRequestId();
    packet.payload = QJsonDocument(json).toJson(QJsonDocument::Compact);
    // requestId -> seq：响应到达时反查并即删，避免遗留条目无界增长
    m_fileSeqByRequestId.insert(static_cast<qint64>(packet.requestId), seq);
    sendPacket(packet);
}

void NetworkManager::handleFileUploadCreateResponse(const Packet &packet)
{
    const qint64 requestId = static_cast<qint64>(packet.requestId);
    const auto it = m_fileSeqByRequestId.constFind(requestId);
    if (it == m_fileSeqByRequestId.constEnd()) {
        return;  // 非本端发起，或任务已取消/超时清理
    }
    const qint64 seq = it.value();
    m_fileSeqByRequestId.remove(requestId);

    const QJsonObject response = QJsonDocument::fromJson(packet.payload).object();
    const int code = response.value("code").toInt(static_cast<int>(ErrorCode::InternalError));
    if (code != static_cast<int>(ErrorCode::Ok)) {
        m_fileTransfer->onUploadCreated(seq, false, 0, QString(),
                                        response.value("message").toString());
        return;
    }
    const QJsonObject data = response.value("data").toObject();
    m_fileTransfer->onUploadCreated(seq, true,
                                    data.value("fileId").toVariant().toLongLong(),
                                    data.value("uploadTicket").toString(), QString());
}

void NetworkManager::handleFileUploadQueryResponse(const Packet &packet)
{
    const qint64 requestId = static_cast<qint64>(packet.requestId);
    const auto it = m_fileSeqByRequestId.constFind(requestId);
    if (it == m_fileSeqByRequestId.constEnd()) {
        return;
    }
    const qint64 seq = it.value();
    m_fileSeqByRequestId.remove(requestId);

    const QJsonObject response = QJsonDocument::fromJson(packet.payload).object();
    const int code = response.value("code").toInt(static_cast<int>(ErrorCode::InternalError));
    if (code != static_cast<int>(ErrorCode::Ok)) {
        m_fileTransfer->onUploadQueried(seq, false, QList<int>(),
                                        response.value("message").toString());
        return;
    }
    QList<int> received;
    const QJsonArray chunks = response.value("data").toObject().value("receivedChunks").toArray();
    received.reserve(chunks.size());
    for (const QJsonValue &value : chunks) {
        received.append(value.toVariant().toInt());
    }
    m_fileTransfer->onUploadQueried(seq, true, received, QString());
}

void NetworkManager::handleFileUploadCompleteResponse(const Packet &packet)
{
    const qint64 requestId = static_cast<qint64>(packet.requestId);
    const auto it = m_fileSeqByRequestId.constFind(requestId);
    if (it == m_fileSeqByRequestId.constEnd()) {
        return;
    }
    const qint64 seq = it.value();
    m_fileSeqByRequestId.remove(requestId);

    const QJsonObject response = QJsonDocument::fromJson(packet.payload).object();
    const int code = response.value("code").toInt(static_cast<int>(ErrorCode::InternalError));
    // 分片未齐备（3016）属可恢复：把已收分片回给引擎，它只补传缺的那几片
    if (code == static_cast<int>(ErrorCode::FileUploadIncomplete)) {
        QList<int> received;
        const QJsonArray chunks =
            response.value("data").toObject().value("receivedChunks").toArray();
        for (const QJsonValue &value : chunks) {
            received.append(value.toVariant().toInt());
        }
        m_fileTransfer->onUploadQueried(seq, true, received, QString());
        return;
    }
    m_fileTransfer->onUploadCompleted(seq, code == static_cast<int>(ErrorCode::Ok),
                                      response.value("message").toString());
}

void NetworkManager::handleFileUploadCancelResponse(const Packet &packet)
{
    const qint64 requestId = static_cast<qint64>(packet.requestId);
    const auto it = m_fileSeqByRequestId.constFind(requestId);
    if (it == m_fileSeqByRequestId.constEnd()) {
        return;
    }
    const qint64 seq = it.value();
    m_fileSeqByRequestId.remove(requestId);

    const QJsonObject response = QJsonDocument::fromJson(packet.payload).object();
    const int code = response.value("code").toInt(static_cast<int>(ErrorCode::InternalError));
    m_fileTransfer->onUploadCancelled(seq, code == static_cast<int>(ErrorCode::Ok),
                                      response.value("message").toString());
}

void NetworkManager::handleFileDownloadTicketResponse(const Packet &packet)
{
    const qint64 requestId = static_cast<qint64>(packet.requestId);
    const auto it = m_fileSeqByRequestId.constFind(requestId);
    if (it == m_fileSeqByRequestId.constEnd()) {
        return;
    }
    const qint64 seq = it.value();
    m_fileSeqByRequestId.remove(requestId);

    const QJsonObject response = QJsonDocument::fromJson(packet.payload).object();
    const int code = response.value("code").toInt(static_cast<int>(ErrorCode::InternalError));
    if (code != static_cast<int>(ErrorCode::Ok)) {
        m_fileTransfer->onDownloadTicket(seq, false, QString(), 0, 0, 0, QString(),
                                         response.value("message").toString());
        return;
    }
    const QJsonObject data = response.value("data").toObject();
    m_fileTransfer->onDownloadTicket(seq, true, data.value("downloadTicket").toString(),
                                     data.value("sizeBytes").toVariant().toLongLong(),
                                     data.value("chunkSize").toVariant().toLongLong(),
                                     data.value("chunkCount").toVariant().toInt(),
                                     data.value("sha256").toString(), QString());
}

void NetworkManager::attachFileInfo(QJsonObject &message)
{
    if (message.value("undecryptable").toBool()) {
        return;
    }
    const QString plain = message.value("content").toString();
    // 先看正文形态再看 fileId：本地缓存表没有 file_id 列，从缓存回填的
    // 消息不带该字段，若以它为前置条件就会漏登记并把清单渲染成正文
    if (!XYChat::Protocol::looksLikeFileManifest(plain)) {
        return;
    }
    bool ok = false;
    const XYChat::Protocol::FileManifest manifest =
        XYChat::Protocol::decodeFileManifest(plain, &ok);
    if (!ok || !manifest.isValid()) {
        return;
    }
    // fileId 以服务端字段为权威，缺失时回退到清单内的值（两者应一致）
    qint64 fileId = message.value("fileId").toVariant().toLongLong();
    if (fileId <= 0) {
        fileId = manifest.fileId;
    }
    if (fileId <= 0 || (message.contains("fileId") && fileId != manifest.fileId)) {
        return;
    }
    message["fileId"] = fileId;
    const qint64 messageId = message.value("messageId").toVariant().toLongLong();
    if (messageId > 0) {
        // 清单（含文件密钥）只登记到传输引擎，不经 QML。这一句是离线补收、
        // 历史翻页、本地缓存回填与本人发送四条路径唯一的登记入口
        m_fileTransfer->registerIncomingFile(messageId, plain);
    }
    // 展示字段脱敏后交给 QML：给出去的字段不包含 key/iv
    message["isFileMessage"] = true;
    message["fileName"] = manifest.name;
    message["fileMime"] = manifest.mime;
    message["fileSizeBytes"] = manifest.plainSize;
    message["fileSha256"] = manifest.sha256Hex;
    // 密文体积与摘要都不是秘密（服务端也知道），UI 需要两者才能判定
    // 本地密文缓存是否命中，以免每次渲染都去发起下载
    message["fileCipherSize"] = manifest.cipherSize;
    // M8.3: 图片尺寸与内联缩略图。缩略图是 JPEG 字节，**不含任何密钥**，
    // 因此可以进 QML：它随清单经 E2EE 到达（服务端不可见），UI 靠它
    // 在下载原图之前就能展示预览。以 base64 交给 QML 拼 data URL
    message["fileWidth"] = manifest.width;
    message["fileHeight"] = manifest.height;
    // M8.3a 欠账补齐：清单 thumb 为空（M8.3a 之前发送的历史图片消息）且原图
    // 已下载时，从本地密文缓存解密生成缩略图并缓存（首次生成后后续命中缓存）。
    // 仅对图片做：音视频封面走上传时生成的清单 thumb，不在接收端补
    QByteArray thumbBytes = manifest.thumbnail;
    if (thumbBytes.isEmpty() && messageId > 0
        && manifest.mime.startsWith(QLatin1String("image/"))) {
        thumbBytes = m_fileTransfer->localThumbnailForMessage(messageId);
    }
    if (!thumbBytes.isEmpty()) {
        message["fileThumb"] = QString::fromLatin1(thumbBytes.toBase64());
    }
    // M8.3b: 音视频时长（毫秒）。同样不含密钥，可进 QML 供气泡展示时长标签。
    // 为 0 表示未知（音频提取失败或非音视频），UI 据此隐藏时长标签
    message["fileDurationMs"] = manifest.durationMs;
}

void NetworkManager::sanitizeForUi(QJsonObject &message)
{
    attachFileInfo(message);
    // 兜底防线：即使 fileId 缺失、清单解析失败或字段不自洽，只要正文形态
    // 像清单就一律置空。宁可不展示一个附件，也绝不把含密钥的 JSON 交给
    // JS 引擎（字符串一旦进入 JS 堆就无法可靠清零，且会被截图/日志/调试器捕获）
    const bool alreadyMarked = message.value("isFileMessage").toBool();
    if (alreadyMarked
        || XYChat::Protocol::looksLikeFileManifest(message.value("content").toString())) {
        message["isFileMessage"] = true;
        // 只在兜底命中时告警（形态像清单但 attachFileInfo 未能正常解析/登记）：
        // 正常路径的脱敏是预期行为，记 Warning 会让告警失去指示意义
        // 并淹没日志。不记正文本身（它含密钥），只记足以定位的元信息
        if (!alreadyMarked && !message.value("content").toString().isEmpty()) {
            qWarning() << "[NetMgr] Scrubbing an unparsed file manifest before it reaches QML,"
                       << "messageId=" << message.value("messageId").toVariant().toLongLong();
        }
        message["content"] = QString();
    }
}

QJsonArray NetworkManager::sanitizeArrayForUi(const QJsonArray &messages)
{
    QJsonArray result;
    for (const QJsonValue &value : messages) {
        QJsonObject msg = value.toObject();
        sanitizeForUi(msg);
        result.append(msg);
    }
    return result;
}

void NetworkManager::handleNewMessageNotification(const Packet &packet)
{
    QJsonObject msg = QJsonDocument::fromJson(packet.payload).object();

    // M7b: 群聊 Sender Key 分发消息只处理、不展示、不落库
    const QString contentType = msg.value("contentType").toString();
    const QString content = msg.value("content").toString();
    if (contentType == QLatin1String("sender_key_distribution")
        || GroupE2eeCrypto::looksLikeDistribution(content)) {
        processGroupSenderKeyDistribution(msg);
        return;
    }

    // M6: 实时推送的消息先解密再交给 UI
    decryptMessageObject(msg);
    // M8.2: 文件消息：登记清单（含密钥，仅 C++ 侧）并补上脱敏展示字段
    attachFileInfo(msg);
    // M6.5: 新消息写入本地缓存，并更新会话预览/未读数（仅更新已存在会话）
    m_localStore.upsertMessage(msg);
    {
        const qint64 convId = msg.value("conversationId").toVariant().toLongLong();
        // M7a: 群系统消息预览用可读摘要，避免结构化 JSON 直接展示
        const bool isSystem = msg.value("contentType").toString() == "system";
        // M8.2: 文件消息的正文是清单 JSON，预览必须用文件名摘要，
        // 否则会话列表会展示一大段含密钥的 JSON
        const QString preview = msg.value("undecryptable").toBool()
            ? "[Encrypted message]"
            : (msg.value("isFileMessage").toBool()
                   ? "[File] " + msg.value("fileName").toString()
                   : (isSystem ? systemMessageSummary(msg.value("content").toString())
                               : msg.value("content").toString()));
        m_localStore.bumpConversationPreview(convId, preview, true);
    }
    // M8.2: 文件消息的正文（清单）含 32 字节文件密钥，绝不得进 QML/JS 引擎。
    // 已落库的是原始清单（本地库加密），emit 前经统一脱敏出口取副本
    QJsonObject uiMsg = msg;
    sanitizeForUi(uiMsg);
    emit newMessageReceived(uiMsg);

    // 自动发送已送达确认
    const qint64 msgId = msg.value("messageId").toVariant().toLongLong();
    if (msgId > 0) {
        ackMessage(msgId, "delivered");
    }
}

// M5.5: 消息状态更新推送
void NetworkManager::handleMessageStatusUpdate(const Packet &packet)
{
    const QJsonObject msg = QJsonDocument::fromJson(packet.payload).object();
    const qint64 msgId = msg.value("messageId").toVariant().toLongLong();
    const QString status = msg.value("status").toString();
    if (msgId > 0 && !status.isEmpty()) {
        // M6.5: 同步更新本地缓存中的消息状态
        m_localStore.updateMessageStatus(msgId, status);
        emit messageStatusChanged(msgId, status);
    }
}

// M9: 已读游标推送（同账号其他设备已读）
void NetworkManager::handleReadCursorNotification(const Packet &packet)
{
    const QJsonObject obj = QJsonDocument::fromJson(packet.payload).object();
    applyReadCursor(obj.value("conversationId").toVariant().toLongLong(),
                    obj.value("readMessageId").toVariant().toLongLong());
}

// M9: 应用已读游标——更新本地缓存（未读清零 + 对方消息标记已读）并通知 UI；
// 实时推送（ReadCursorNotification）与离线补偿（sync_events read_cursor）共用
void NetworkManager::applyReadCursor(qint64 conversationId, qint64 readMessageId)
{
    if (conversationId <= 0 || readMessageId <= 0) {
        return;
    }
    if (m_localStore.isOpen()) {
        m_localStore.markConversationRead(conversationId, readMessageId, m_userId);
        // 会话列表未读角标已变，重新 emit 缓存会话刷新侧边栏
        emit conversationsResult(m_localStore.loadConversations());
    }
    emit readCursorAdvanced(conversationId, readMessageId);
}

// M5.5: 账号级增量同步
void NetworkManager::syncEvents(qint64 afterSeq, int limit)
{
    if (m_state != ConnectionState::Authenticated) return;

    QJsonObject json;
    json["type"] = "sync_events";
    json["afterSeq"] = afterSeq;
    json["limit"] = limit;
    addReplayProtection(json);

    Packet packet;
    packet.messageType = MessageType::SyncEventsRequest;
    packet.requestId = nextRequestId();
    packet.payload = QJsonDocument(json).toJson(QJsonDocument::Compact);
    m_pendingSyncEventsRequestId = packet.requestId;
    sendPacket(packet);
}

void NetworkManager::handleSyncEventsResponse(const Packet &packet)
{
    if (packet.requestId != m_pendingSyncEventsRequestId) return;
    m_pendingSyncEventsRequestId = 0;

    const QJsonObject response = QJsonDocument::fromJson(packet.payload).object();
    if (response.value("code").toInt() == static_cast<int>(ErrorCode::Ok)) {
        const QJsonObject data = response.value("data").toObject();

        // M9: 落后于服务端清理水位——afterSeq 与水位线之间的事件已被清理，
        // 增量拉取不可得，回退全量：重置游标到最新 seq 并重拉会话列表，
        // 历史消息在用户打开对应会话时经 syncMessages(afterId=0) 全量补齐
        if (data.value("needsFullSync").toBool()) {
            const qint64 fullSyncSeq = data.value("fullSyncSeq").toVariant().toLongLong();
            qInfo() << "[NetMgr] sync_events behind prune watermark; full resync to seq"
                    << fullSyncSeq;
            if (m_localStore.isOpen() && fullSyncSeq > m_localStore.syncCursor()) {
                m_localStore.setSyncCursor(fullSyncSeq);
            }
            getConversations();
            return;
        }

        // M6: 解密事件流中的 message 事件正文
        QJsonArray events = data.value("events").toArray();
        for (QJsonValueRef value : events) {
            QJsonObject event = value.toObject();
            if (event.value("type").toString() == "message") {
                QJsonObject payload = event.value("payload").toObject();
                const QString ct = payload.value("contentType").toString();
                const QString c = payload.value("content").toString();
                // M7b: 分发消息直接处理，不需要解密展示
                if (ct == QLatin1String("sender_key_distribution")
                    || GroupE2eeCrypto::looksLikeDistribution(c)) {
                    processGroupSenderKeyDistribution(payload);
                    // 分发消息不展示：清空 content，避免 envelope 原文随 eventsSynced 外发
                    payload["content"] = QString();
                } else {
                    decryptMessageObject(payload);
                }
                event["payload"] = payload;
                value = event;
            }
        }
        // M6.5: 事件写入本地缓存并推进游标（hasMore 时自动续拉）
        const qint64 lastSeq = data.value("lastSeq").toVariant().toLongLong();
        const bool hasMore = data.value("hasMore").toBool();
        ingestSyncEvents(events, lastSeq, hasMore);
        // M8.2: 落库与游标推进用的是原始事件（保留清单），emit 给 UI 的用脱敏
        // 副本：离线补收的文件消息正文同样是清单，不经此处就会把密钥渲染进气泡
        QJsonArray uiEvents;
        for (const QJsonValue &eventValue : events) {
            QJsonObject event = eventValue.toObject();
            QJsonObject payload = event.value("payload").toObject();
            if (!payload.isEmpty()) {
                sanitizeForUi(payload);
                event["payload"] = payload;
            }
            uiEvents.append(event);
        }
        emit eventsSynced(uiEvents, lastSeq, hasMore);
    }
}

// M6.5: 本地持久化缓存
void NetworkManager::openLocalStore()
{
    if (m_username.isEmpty() || m_localDeviceId.isEmpty()) {
        return;
    }
    // 切换账号：清除旧账号用户可见数据后关闭（保留其解密缓存，
    // 该账号重登时仍需依靠它解密），再打开新账号本地库
    if (m_localStore.isOpen() && m_localStore.username() != m_username) {
        m_localStore.clearUserData();
        m_localStore.close();
    }
    if (!m_localStore.isOpen() && !m_localStore.open(m_username, m_localDeviceId)) {
        qWarning() << "[NetMgr] LocalStore unavailable, running without local cache";
        return;
    }

    // M6 遗留解密缓存一次性迁入 LocalStore（幂等，无文件时为空操作）
    m_localStore.importLegacyDecryptCache(m_username, m_localDeviceId);

    // 持久化 outbox 载入内存（幂等键去重），由后续 flushOutbox 重发
    const QList<LocalStore::OutboxItem> persisted = m_localStore.loadOutbox();
    for (const LocalStore::OutboxItem &item : persisted) {
        bool exists = false;
        for (const OutboxItem &mem : std::as_const(m_outbox)) {
            if (mem.clientMessageId == item.clientMessageId) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            m_outbox.append({item.clientMessageId, item.toUserId,
                             item.conversationId, item.content});
        }
    }

    emitCachedConversations();

    // 基于 sync_events 游标增量同步，补齐离线期间错过的消息/回执
    syncEvents(m_localStore.syncCursor());
}

void NetworkManager::emitCachedConversations()
{
    if (!m_localStore.isOpen()) {
        return;
    }
    const QJsonArray cached = m_localStore.loadConversations();
    if (!cached.isEmpty()) {
        // 立即展示上一会话周期的会话列表，随后服务端数据到达时刷新
        emit conversationsResult(cached);
    }
}

void NetworkManager::ingestSyncEvents(const QJsonArray &events, qint64 lastSeq, bool hasMore)
{
    if (!m_localStore.isOpen()) {
        return;
    }

    // P1-3: 离线成员变更补偿集合（去重后批次结束统一触发 healing）
    QSet<qint64> healConvs;

    for (const QJsonValue &value : events) {
        const QJsonObject event = value.toObject();
        const QString type = event.value("type").toString();
        const QJsonObject payload = event.value("payload").toObject();

        if (type == "message") {
            QJsonObject msg = payload;
            // M7b: 分发消息不入库
            const QString ct = msg.value("contentType").toString();
            const QString c = msg.value("content").toString();
            if (ct == QLatin1String("sender_key_distribution")
                || GroupE2eeCrypto::looksLikeDistribution(c)) {
                processGroupSenderKeyDistribution(msg);
                continue;
            }
            // 事件流无状态字段：按发送方推导初始状态
            const bool fromSelf = msg.value("senderId").toVariant().toLongLong() == m_userId;
            msg["status"] = fromSelf ? "sent"
                                     : "delivered";
            // 本机已成功投递的消息同步移除 outbox（ACK 丢失时的兜底）
            const QString cmid = msg.value("clientMessageId").toString();
            if (fromSelf && !cmid.isEmpty()) {
                for (int i = m_outbox.size() - 1; i >= 0; --i) {
                    if (m_outbox.at(i).clientMessageId == cmid) {
                        m_outbox.removeAt(i);
                        break;
                    }
                }
                m_localStore.removeOutboxItem(cmid);
            }
            m_localStore.upsertMessage(msg);
        } else if (type == "receipt") {
            m_localStore.updateMessageStatus(
                payload.value("messageId").toVariant().toLongLong(),
                payload.value("status").toString());
        } else if (type == "read_cursor") {
            // M9: 已读者自身其他设备的已读游标（离线补偿）
            applyReadCursor(payload.value("conversationId").toVariant().toLongLong(),
                            payload.value("readMessageId").toVariant().toLongLong());
        } else if (type == "group_changed") {
            // P1-3: 离线期间的群成员变更补偿：收集需 healing 的群，批次结束后统一触发
            const qint64 convId = payload.value("conversationId").toVariant().toLongLong();
            const QString changeType = payload.value("changeType").toString();
            const qint64 targetUserId = payload.value("targetUserId").toVariant().toLongLong();
            const qint64 operatorId = payload.value("operatorId").toVariant().toLongLong();
            const bool membershipChanged = changeType == "member_added"
                || changeType == "member_removed"
                || changeType == "member_left";
            const bool selfRemoved =
                (changeType == "member_removed" && targetUserId == m_userId)
                || (changeType == "member_left" && operatorId == m_userId);
            if (convId > 0 && membershipChanged) {
                if (selfRemoved) {
                    m_groupSenderKeys.remove(convId);
                    healConvs.remove(convId);
                } else {
                    healConvs.insert(convId);
                }
            }
        } else if (type == "conversation_prefs") {
            // M9 特性栈：离线期间的会话偏好变更补偿（本人其他设备设置）
            applyConversationPrefs(payload.value("conversationId").toVariant().toLongLong(),
                                   payload.value("pinned").toBool(),
                                   payload.value("muted").toBool());
        } else if (type == "message_edited") {
            // M9 特性栈：离线期间的消息编辑补偿，直接解密新正文并更新本地缓存、
            // 通知 UI。本设备发起的编辑跳过（已在响应路径处理）。同机多账号下
            // deviceId 相同，须连同 senderId 一并比对，否则同机接收方会误跳过
            const qint64 senderId = payload.value("senderId").toVariant().toLongLong();
            const QString originDeviceId = payload.value("originDeviceId").toString();
            if (senderId == m_userId && !originDeviceId.isEmpty()
                && originDeviceId == m_localDeviceId) {
                continue;
            }
            const qint64 msgId = payload.value("messageId").toVariant().toLongLong();
            const qint64 convId = payload.value("conversationId").toVariant().toLongLong();
            const QString editedAt = payload.value("editedAt").toString();
            // 直接解密编辑后的新正文（不预先清缓存，失败路径保留既有可读缓存/正文）
            QString plaintext;
            const bool decrypted = decryptEditContent(payload, plaintext);
            if (decrypted && m_localStore.isOpen()) {
                m_localStore.updateMessageContent(msgId, plaintext, editedAt);
                emit messageEdited(convId, msgId, plaintext, editedAt);
                continue;
            }
            if (m_localStore.isOpen()) {
                // 新正文解不出：离线重放时一次性预密钥已消费 / 群 ratchet 已推进。
                // 绝不写空覆盖既有可读正文；缓存保留，重登后仍可恢复可读文本
                const QString existing = m_localStore.loadMessageContent(msgId);
                if (!existing.isEmpty()) {
                    m_localStore.markMessageEdited(msgId, editedAt);
                    emit messageEdited(convId, msgId, existing, editedAt);
                }
            }
        } else if (type == "message_deleted") {
            // M9 特性栈：离线期间的消息删除补偿（本设备发起的删除同样跳过；
            // 同机多账号下 deviceId 相同，须连同 senderId 一并比对）
            const qint64 senderId = payload.value("senderId").toVariant().toLongLong();
            const QString originDeviceId = payload.value("originDeviceId").toString();
            if (senderId == m_userId && !originDeviceId.isEmpty()
                && originDeviceId == m_localDeviceId) {
                continue;
            }
            const qint64 msgId = payload.value("messageId").toVariant().toLongLong();
            const qint64 convId = payload.value("conversationId").toVariant().toLongLong();
            if (m_localStore.isOpen()) {
                m_localStore.markMessageDeleted(msgId);
            }
            emit messageDeleted(convId, msgId);
        }
        // contact_added 等事件忽略：联系人列表按需从服务端拉取
    }

    // P1-3: 触发离线补偿的 Sender-Key healing（去重后逐群，受单发槽位约束由队列推进）
    for (qint64 convId : std::as_const(healConvs)) {
        healGroupSenderKey(convId);
    }

    // 游标单调前进
    if (lastSeq > m_localStore.syncCursor()) {
        m_localStore.setSyncCursor(lastSeq);
    }
    // 仍有事件时继续增量拉取，直至追平
    if (hasMore && m_state == ConnectionState::Authenticated) {
        syncEvents(lastSeq);
    }
}

// QML 辅助方法
QString NetworkManager::encryptPassword(const QString &password) const
{
    return EncryptionManager::encryptPassword(password);
}

QVariantList NetworkManager::toVariantList(const QJsonArray &array) const
{
    QVariantList result;
    for (const auto &val : array) {
        result.append(val.toVariant());
    }
    return result;
}

// M7a: 群组操作

void NetworkManager::createGroup(const QString &name, const QVariantList &memberIds)
{
    if (m_state != ConnectionState::Authenticated) return;

    QJsonObject json;
    json["type"] = "create_group";
    json["name"] = name;
    QJsonArray members;
    for (const QVariant &v : memberIds) {
        const qint64 id = v.toLongLong();
        if (id > 0 && id != m_userId) {
            members.append(id);
        }
    }
    json["memberIds"] = members;
    addReplayProtection(json);

    Packet packet;
    packet.messageType = MessageType::CreateGroupRequest;
    packet.requestId = nextRequestId();
    packet.payload = QJsonDocument(json).toJson(QJsonDocument::Compact);
    m_pendingCreateGroupRequestId = packet.requestId;
    sendPacket(packet);
}

void NetworkManager::inviteGroupMembers(qint64 conversationId, const QVariantList &userIds)
{
    if (m_state != ConnectionState::Authenticated) return;

    QJsonObject json;
    json["type"] = "invite_group_members";
    json["conversationId"] = conversationId;
    QJsonArray users;
    for (const QVariant &v : userIds) {
        const qint64 id = v.toLongLong();
        if (id > 0 && id != m_userId) {
            users.append(id);
        }
    }
    json["userIds"] = users;
    addReplayProtection(json);

    Packet packet;
    packet.messageType = MessageType::InviteGroupMembersRequest;
    packet.requestId = nextRequestId();
    packet.payload = QJsonDocument(json).toJson(QJsonDocument::Compact);
    m_pendingInviteGroupRequestId = packet.requestId;
    sendPacket(packet);
}

void NetworkManager::leaveGroup(qint64 conversationId)
{
    if (m_state != ConnectionState::Authenticated) return;

    QJsonObject json;
    json["type"] = "leave_group";
    json["conversationId"] = conversationId;
    addReplayProtection(json);

    Packet packet;
    packet.messageType = MessageType::LeaveGroupRequest;
    packet.requestId = nextRequestId();
    packet.payload = QJsonDocument(json).toJson(QJsonDocument::Compact);
    m_pendingLeaveGroupRequestId = packet.requestId;
    sendPacket(packet);
}

void NetworkManager::kickGroupMember(qint64 conversationId, qint64 userId)
{
    if (m_state != ConnectionState::Authenticated) return;

    QJsonObject json;
    json["type"] = "kick_group_member";
    json["conversationId"] = conversationId;
    json["userId"] = userId;
    addReplayProtection(json);

    Packet packet;
    packet.messageType = MessageType::KickGroupMemberRequest;
    packet.requestId = nextRequestId();
    packet.payload = QJsonDocument(json).toJson(QJsonDocument::Compact);
    m_pendingKickGroupRequestId = packet.requestId;
    sendPacket(packet);
}

void NetworkManager::getGroupInfo(qint64 conversationId)
{
    if (m_state != ConnectionState::Authenticated) return;

    QJsonObject json;
    json["type"] = "get_group_info";
    json["conversationId"] = conversationId;
    addReplayProtection(json);

    Packet packet;
    packet.messageType = MessageType::GetGroupInfoRequest;
    packet.requestId = nextRequestId();
    packet.payload = QJsonDocument(json).toJson(QJsonDocument::Compact);
    m_pendingGetGroupInfoRequestId = packet.requestId;
    sendPacket(packet);
}

// M7a: 群组响应

void NetworkManager::handleCreateGroupResponse(const Packet &packet)
{
    if (packet.requestId != m_pendingCreateGroupRequestId) return;
    m_pendingCreateGroupRequestId = 0;

    const QJsonObject response = QJsonDocument::fromJson(packet.payload).object();
    if (response.value("code").toInt() != static_cast<int>(ErrorCode::Ok)) {
        emit groupRequestFailed(response.value("message").toString("Failed to create group"));
        return;
    }

    const QJsonObject data = response.value("data").toObject();
    const qint64 conversationId = data.value("conversationId").toVariant().toLongLong();
    const QString name = data.value("name").toString();

    // 新群立即写入本地缓存（缓存先行，服务端刷新随后覆盖）
    QJsonObject conv = data;
    conv["type"] = "group";
    conv["unreadCount"] = 0;
    m_localStore.upsertConversation(conv);
    getConversations();

    emit groupCreated(conversationId, name);
}

void NetworkManager::handleInviteGroupMembersResponse(const Packet &packet)
{
    if (packet.requestId != m_pendingInviteGroupRequestId) return;
    m_pendingInviteGroupRequestId = 0;

    const QJsonObject response = QJsonDocument::fromJson(packet.payload).object();
    if (response.value("code").toInt() != static_cast<int>(ErrorCode::Ok)) {
        emit groupRequestFailed(response.value("message").toString("Failed to invite members"));
        return;
    }

    const QJsonObject data = response.value("data").toObject();
    getConversations();
    emit groupMembersInvited(data.value("conversationId").toVariant().toLongLong());
}

void NetworkManager::handleLeaveGroupResponse(const Packet &packet)
{
    if (packet.requestId != m_pendingLeaveGroupRequestId) return;
    m_pendingLeaveGroupRequestId = 0;

    const QJsonObject response = QJsonDocument::fromJson(packet.payload).object();
    if (response.value("code").toInt() != static_cast<int>(ErrorCode::Ok)) {
        emit groupRequestFailed(response.value("message").toString("Failed to leave group"));
        return;
    }

    const qint64 conversationId =
        response.value("data").toObject().value("conversationId").toVariant().toLongLong();
    // P1-3: 本端退群，清除内存与本地 sender key（chain key / Ed25519 私钥），先 wipe 再移除
    auto keyIt = m_groupSenderKeys.find(conversationId);
    if (keyIt != m_groupSenderKeys.end()) {
        SecureMemory::wipe(keyIt->chainKey);
        SecureMemory::wipe(keyIt->privateSigningKey);
        m_groupSenderKeys.erase(keyIt);
    }
    if (m_localStore.isOpen()) {
        m_localStore.removeSenderKeysForGroup(conversationId);
    }
    getConversations();
    emit groupLeft(conversationId);
}

void NetworkManager::handleKickGroupMemberResponse(const Packet &packet)
{
    if (packet.requestId != m_pendingKickGroupRequestId) return;
    m_pendingKickGroupRequestId = 0;

    const QJsonObject response = QJsonDocument::fromJson(packet.payload).object();
    if (response.value("code").toInt() != static_cast<int>(ErrorCode::Ok)) {
        emit groupRequestFailed(response.value("message").toString("Failed to remove member"));
        return;
    }

    const QJsonObject data = response.value("data").toObject();
    emit groupMemberKicked(data.value("conversationId").toVariant().toLongLong(),
                           data.value("removedUserId").toVariant().toLongLong());
}

void NetworkManager::handleGetGroupInfoResponse(const Packet &packet)
{
    if (packet.requestId != m_pendingGetGroupInfoRequestId) return;
    m_pendingGetGroupInfoRequestId = 0;

    const QJsonObject response = QJsonDocument::fromJson(packet.payload).object();
    if (response.value("code").toInt() != static_cast<int>(ErrorCode::Ok)) {
        emit groupRequestFailed(response.value("message").toString("Failed to get group info"));
        return;
    }

    emit groupInfoResult(response.value("data").toObject());
}

// M7a: 群变更推送：刷新会话列表并通知 UI（被移除/退群后群从列表消失）
void NetworkManager::handleGroupChangedNotification(const Packet &packet)
{
    const QJsonObject payload = QJsonDocument::fromJson(packet.payload).object();
    const qint64 convId = payload.value("conversationId").toVariant().toLongLong();
    const QString changeType = payload.value("changeType").toString();
    const qint64 targetUserId = payload.value("targetUserId").toVariant().toLongLong();
    const qint64 operatorId = payload.value("operatorId").toVariant().toLongLong();

    getConversations();

    // P1-3: 群成员变更触发 Sender-Key healing（轮换 + 重分发）
    const bool membershipChanged = changeType == "member_added"
        || changeType == "member_removed"
        || changeType == "member_left";
    const bool selfRemoved =
        (changeType == "member_removed" && targetUserId == m_userId)
        || (changeType == "member_left" && operatorId == m_userId);
    if (membershipChanged && convId > 0) {
        if (selfRemoved) {
            // 本端已非成员：清除内存 sender key（对方轮换后旧密钥无法解密新消息）
            m_groupSenderKeys.remove(convId);
        } else {
            healGroupSenderKey(convId);
        }
    }

    emit groupChanged(payload);
}

// ── M9 特性栈：会话偏好（置顶/免打扰） ──
void NetworkManager::setConversationPrefs(qint64 conversationId, bool pinned, bool muted)
{
    if (m_state != ConnectionState::Authenticated || conversationId <= 0) {
        return;
    }

    QJsonObject json;
    json["type"] = "set_conversation_prefs";
    json["conversationId"] = conversationId;
    json["pinned"] = pinned;
    json["muted"] = muted;
    addReplayProtection(json);

    Packet packet;
    packet.messageType = MessageType::SetConversationPrefsRequest;
    packet.requestId = nextRequestId();
    packet.payload = QJsonDocument(json).toJson(QJsonDocument::Compact);
    m_pendingSetPrefsRequestId = packet.requestId;
    sendPacket(packet);
}

void NetworkManager::handleSetConversationPrefsResponse(const Packet &packet)
{
    if (packet.requestId != m_pendingSetPrefsRequestId) return;
    m_pendingSetPrefsRequestId = 0;

    const QJsonObject response = QJsonDocument::fromJson(packet.payload).object();
    if (response.value("code").toInt() != static_cast<int>(ErrorCode::Ok)) {
        qWarning() << "[NetMgr] set_conversation_prefs failed:"
                   << response.value("message").toString();
        return;
    }

    const QJsonObject data = response.value("data").toObject();
    const qint64 conversationId = data.value("conversationId").toVariant().toLongLong();
    const bool pinned = data.value("pinned").toBool();
    const bool muted = data.value("muted").toBool();
    applyConversationPrefs(conversationId, pinned, muted);
}

// 实时推送：本人其他设备设置了偏好（多端同步）
void NetworkManager::handleConversationPrefsNotification(const Packet &packet)
{
    const QJsonObject payload = QJsonDocument::fromJson(packet.payload).object();
    const qint64 conversationId = payload.value("conversationId").toVariant().toLongLong();
    const bool pinned = payload.value("pinned").toBool();
    const bool muted = payload.value("muted").toBool();
    applyConversationPrefs(conversationId, pinned, muted);
}

// 本地缓存更新 + 通知 UI（响应与推送、sync_events 事件共用）
void NetworkManager::applyConversationPrefs(qint64 conversationId, bool pinned, bool muted)
{
    if (conversationId <= 0) {
        return;
    }
    if (m_localStore.isOpen()) {
        m_localStore.setConversationPrefs(conversationId, pinned, muted);
        emit conversationsResult(m_localStore.loadConversations());
    }
    emit conversationPrefsChanged(conversationId, pinned, muted);
}

// ── M9 特性栈：消息编辑/删除 ──
void NetworkManager::editMessage(qint64 conversationId, qint64 peerUserId,
                                 qint64 messageId, const QString &newContent)
{
    if (m_state != ConnectionState::Authenticated || messageId <= 0) {
        return;
    }
    const QString trimmed = newContent.trimmed();
    if (trimmed.isEmpty()) {
        emit messageEditFailed("Edited content is empty");
        return;
    }

    if (conversationId > 0 && peerUserId == 0) {
        // 群聊：同步用 Sender-Key 重新加密后直接提交（requestId 登记上下文，多编辑并发不丢）
        const QString envelope = encryptGroupMessage(conversationId, newContent);
        if (envelope.isEmpty()) {
            emit messageEditFailed("Failed to encrypt edited group message");
            return;
        }
        sendEditMessageRequest(messageId, conversationId, envelope, "e2ee_group", newContent);
    } else if (peerUserId > 0) {
        // 私聊：入队等待拉取对方密钥包后加密提交（M9 欠账修复：队列化，
        // 避免在途期间再次编辑覆盖单槽上下文）
        PrivateEditWait w;
        w.messageId = messageId;
        w.conversationId = conversationId;
        w.peerUserId = peerUserId;
        w.plaintext = newContent;
        m_privateEditQueue.enqueue(w);
        pumpPrivateEditFetch();
    } else {
        emit messageEditFailed("Invalid conversation for edit");
    }
}

// M9 欠账修复：仅当无在途编辑 fetch 且传输槽空闲时，为队首私聊编辑发起拉取
void NetworkManager::pumpPrivateEditFetch()
{
    if (m_editFetchInFlight || m_privateEditQueue.isEmpty()) {
        return;
    }
    if (m_pendingFetchKeysRequestId != 0) {
        return; // 传输槽被发送/其他拉取占用，其响应回调中会再次泵送
    }
    m_editFetchInFlight = true;
    sendFetchKeysRequest(m_privateEditQueue.head().peerUserId);
}

quint64 NetworkManager::sendEditMessageRequest(qint64 messageId, qint64 conversationId,
                                               const QString &content, const QString &contentType,
                                               const QString &plaintext)
{
    QJsonObject json;
    json["type"] = "edit_message";
    json["messageId"] = messageId;
    json["content"] = content;
    json["contentType"] = contentType;
    addReplayProtection(json);

    Packet packet;
    packet.messageType = MessageType::EditMessageRequest;
    packet.requestId = nextRequestId();
    packet.payload = QJsonDocument(json).toJson(QJsonDocument::Compact);
    // M9 欠账修复：以 requestId 为键登记上下文（镜像 m_pendingSendByRequestId），
    // 连续编辑的多条响应均能匹配不被丢弃
    m_pendingEdits.insert(packet.requestId, EditContext{messageId, conversationId, plaintext});
    sendPacket(packet);
    return packet.requestId;
}

void NetworkManager::deleteMessage(qint64 messageId)
{
    if (m_state != ConnectionState::Authenticated || messageId <= 0) {
        return;
    }

    QJsonObject json;
    json["type"] = "delete_message";
    json["messageId"] = messageId;
    addReplayProtection(json);

    Packet packet;
    packet.messageType = MessageType::DeleteMessageRequest;
    packet.requestId = nextRequestId();
    packet.payload = QJsonDocument(json).toJson(QJsonDocument::Compact);
    // M9 欠账修复：删除无逐请求上下文（响应 payload 自带 messageId），用集合记录在途
    m_pendingDeleteRequestIds.insert(packet.requestId);
    sendPacket(packet);
}

void NetworkManager::handleEditMessageResponse(const Packet &packet)
{
    // M9 欠账修复：仅处理本端编辑请求的响应（requestId 命中 m_pendingEdits）；
    // 会话成员编辑的实时推送经 MessageEditedNotification (88) 走专用处理器
    auto it = m_pendingEdits.find(packet.requestId);
    if (packet.requestId == 0 || it == m_pendingEdits.end()) {
        return;
    }
    const EditContext ctx = it.value();
    m_pendingEdits.erase(it);

    const QJsonObject response = QJsonDocument::fromJson(packet.payload).object();
    const int code = response.value("code").toInt(static_cast<int>(ErrorCode::Ok));
    if (code != static_cast<int>(ErrorCode::Ok)) {
        emit messageEditFailed(response.value("message").toString("Failed to edit message"));
        return;
    }

    const QJsonObject data = response.value("data").toObject();
    const qint64 conversationId = data.value("conversationId").toVariant().toLongLong();
    const QString editedAt = data.value("editedAt").toString();

    // 本端编辑成功：用本地新明文更新缓存（服务端只回传重新加密的密文，
    // 本地需以明文落库供展示）。先失效旧解密缓存，避免后续同步/重载命中编辑前明文
    const QString plaintext = ctx.plaintext;
    m_decryptCache.remove(ctx.messageId);
    if (m_localStore.isOpen()) {
        m_localStore.clearDecryptedContent(ctx.messageId);
        if (!plaintext.isEmpty()) {
            m_localStore.updateMessageContent(ctx.messageId, plaintext, editedAt);
            m_localStore.saveDecryptedContent(ctx.messageId, plaintext);
        }
    }
    emit messageEdited(conversationId, ctx.messageId, plaintext, editedAt);
}

void NetworkManager::handleMessageEditedNotification(const Packet &packet)
{
    // M9 欠账修复：其他成员（含本人其他设备）编辑的实时推送（requestId=0）
    const QJsonObject data = QJsonDocument::fromJson(packet.payload).object();
    const qint64 messageId = data.value("messageId").toVariant().toLongLong();
    const qint64 conversationId = data.value("conversationId").toVariant().toLongLong();
    const QString editedAt = data.value("editedAt").toString();

    // 发起设备不回显自身操作：本端已在响应路径完成乐观更新，重复处理会在
    // 群聊下用已推进的 ratchet 状态重试解密并误清空正文。
    // deviceId 为机器级（同机多账号共用），必须同时比对 senderId == 本端用户
    const qint64 senderId = data.value("senderId").toVariant().toLongLong();
    const QString originDeviceId = data.value("originDeviceId").toString();
    if (senderId == m_userId && !originDeviceId.isEmpty()
        && originDeviceId == m_localDeviceId) {
        return;
    }

    // 直接解密新 content（不预先清缓存，避免新正文解不出时把既有可读明文/缓存
    // 一并破坏）。senderId 由服务端随事件下发，群聊解密靠它定位 Sender Key
    QString plaintext;
    const bool decrypted = decryptEditContent(data, plaintext);
    if (decrypted && m_localStore.isOpen()) {
        m_localStore.updateMessageContent(messageId, plaintext, editedAt);
        emit messageEdited(conversationId, messageId, plaintext, editedAt);
        return;
    }
    if (m_localStore.isOpen()) {
        // 新正文解不出：一次性预密钥已消费/群 ratchet 已推进的离线重放场景。
        // 绝不写空覆盖既有可读正文（失败路径不清缓存），重登后仍可恢复
        const QString existing = m_localStore.loadMessageContent(messageId);
        if (!existing.isEmpty()) {
            m_localStore.markMessageEdited(messageId, editedAt);
            emit messageEdited(conversationId, messageId, existing, editedAt);
        }
        // 本地确无既有正文：保留占位（不写空、不改 undecryptable）
    }
}

void NetworkManager::handleDeleteMessageResponse(const Packet &packet)
{
    // M9 欠账修复：仅处理本端删除请求的响应（requestId 命中集合）；
    // 其他成员删除推送经 MessageDeletedNotification (89)
    if (packet.requestId == 0 || !m_pendingDeleteRequestIds.contains(packet.requestId)) {
        return;
    }
    m_pendingDeleteRequestIds.remove(packet.requestId);

    const QJsonObject response = QJsonDocument::fromJson(packet.payload).object();
    const int code = response.value("code").toInt(static_cast<int>(ErrorCode::Ok));
    if (code != static_cast<int>(ErrorCode::Ok)) {
        emit messageDeleteFailed(response.value("message").toString("Failed to delete message"));
        return;
    }

    const QJsonObject data = response.value("data").toObject();
    const qint64 messageId = data.value("messageId").toVariant().toLongLong();
    const qint64 conversationId = data.value("conversationId").toVariant().toLongLong();
    if (m_localStore.isOpen()) {
        m_localStore.markMessageDeleted(messageId);
    }
    emit messageDeleted(conversationId, messageId);
}

void NetworkManager::handleMessageDeletedNotification(const Packet &packet)
{
    // M9 欠账修复：其他成员（含本人其他设备）删除的实时推送（requestId=0）
    const QJsonObject data = QJsonDocument::fromJson(packet.payload).object();
    const qint64 messageId = data.value("messageId").toVariant().toLongLong();
    const qint64 conversationId = data.value("conversationId").toVariant().toLongLong();

    // 发起设备不回显自身操作（本端已在响应路径处理）。
    // 同机多账号下 deviceId 相同，须连同 senderId 一并比对，否则接收方会误跳过
    const qint64 senderId = data.value("senderId").toVariant().toLongLong();
    const QString originDeviceId = data.value("originDeviceId").toString();
    if (senderId == m_userId && !originDeviceId.isEmpty()
        && originDeviceId == m_localDeviceId) {
        return;
    }

    if (m_localStore.isOpen()) {
        m_localStore.markMessageDeleted(messageId);
    }
    emit messageDeleted(conversationId, messageId);
}

// M7a: 群系统消息摘要（contentType=system 的结构化正文转可读文本）
QString NetworkManager::systemMessageSummary(const QString &content)
{
    const QJsonObject obj = QJsonDocument::fromJson(content.toUtf8()).object();
    const QString event = obj.value("event").toString();
    if (event == "group_created") {
        return "创建了群组";
    }
    if (event == "member_added") {
        return "新成员加入群聊";
    }
    if (event == "member_removed") {
        return "成员被移出群聊";
    }
    if (event == "member_left") {
        return "成员退出了群聊";
    }
    if (event == "owner_transferred") {
        return "群主已转让";
    }
    return content;
}
