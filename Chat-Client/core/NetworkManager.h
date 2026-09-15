#pragma once

#include <QObject>
#include <QSslSocket>
#include <QSslError>
#include <QTimer>
#include <QJsonObject>
#include <QJsonArray>
#include <QVariant>
#include <QHash>
#include <QSet>
#include <QQueue>

#include "protocol/PacketCodec.h"
#include "encryption/E2eeCrypto.h"
#include "encryption/GroupE2eeCrypto.h"
#include "KeyStorage.h"
#include "LocalStore.h"
#include "FileTransferManager.h"

class NetworkManager : public QObject
{
    Q_OBJECT
    // 暴露给 QML 的只读属性（带 NOTIFY 以支持响应式绑定）
    Q_PROPERTY(ConnectionState state READ state NOTIFY connectionStateChanged)
    Q_PROPERTY(QString sessionToken READ sessionToken NOTIFY sessionChanged)
    Q_PROPERTY(qint64 userId READ userId NOTIFY sessionChanged)
    Q_PROPERTY(QString username READ username NOTIFY sessionChanged)

public:
    enum class ConnectionState
    {
        Disconnected,
        Connecting,
        Connected,
        LoggingIn,
        Authenticated
    };
    Q_ENUM(ConnectionState)

    explicit NetworkManager(QObject *parent = nullptr);

    // 认证操作
    Q_INVOKABLE void login(const QString &username, const QString &password);
    Q_INVOKABLE void registerAccount(const QString &username, const QString &password,
                                     const QString &email = {}, const QString &phone = {});
    Q_INVOKABLE void logout();
    Q_INVOKABLE void renewToken();

    // M3: 用户搜索与联系人
    Q_INVOKABLE void searchUsers(const QString &query);
    Q_INVOKABLE void addContact(qint64 userId);
    Q_INVOKABLE void getContacts();

    // M3: 会话与消息
    Q_INVOKABLE void getConversations();
    // M4.5: 返回客户端幂等键 clientMessageId，供 QML 跟踪乐观消息状态
    Q_INVOKABLE QString sendMessage(qint64 toUserId, const QString &content, qint64 fileId = 0);
    Q_INVOKABLE void ackMessage(qint64 messageId, const QString &status = "delivered");
    Q_INVOKABLE void syncMessages(qint64 conversationId, qint64 afterId = 0, int limit = 100);
    // M5.5: 账号级增量同步
    Q_INVOKABLE void syncEvents(qint64 afterSeq = 0, int limit = 200);

    // M7a: 群组操作（明文群聊）
    Q_INVOKABLE void createGroup(const QString &name, const QVariantList &memberIds);
    Q_INVOKABLE void inviteGroupMembers(qint64 conversationId, const QVariantList &userIds);
    Q_INVOKABLE void leaveGroup(qint64 conversationId);
    Q_INVOKABLE void kickGroupMember(qint64 conversationId, qint64 userId);
    Q_INVOKABLE void getGroupInfo(qint64 conversationId);
    // M7a: 发送群消息（明文，返回幂等键供乐观消息跟踪）
    Q_INVOKABLE QString sendGroupMessage(qint64 conversationId, const QString &content,
                                         qint64 fileId = 0);

    // M9 特性栈：会话偏好（置顶/免打扰）与消息编辑/删除
    Q_INVOKABLE void setConversationPrefs(qint64 conversationId, bool pinned, bool muted);
    // 编辑消息：私聊（peerUserId>0）走 pairwise E2EE 重新加密，群聊（peerUserId=0）
    // 走 Sender-Key 重新加密
    Q_INVOKABLE void editMessage(qint64 conversationId, qint64 peerUserId,
                                 qint64 messageId, const QString &newContent);
    Q_INVOKABLE void deleteMessage(qint64 messageId);

    // M10：“正在输入”指示。客户端节流（typing=true 每会话最快 4s 一次，
    // 避免每次按键都发包）；typing=false（停止）不节流，立即送达
    Q_INVOKABLE void sendTyping(qint64 conversationId, bool typing = true);
    // M10: 会话整表删除（服务端硬删除；成功后清本地缓存并 emit conversationDeleted）
    Q_INVOKABLE void deleteConversation(qint64 conversationId);

    // 状态查询
    ConnectionState state() const { return m_state; }
    QString sessionToken() const { return m_sessionToken; }
    qint64 userId() const { return m_userId; }
    QString username() const { return m_username; }

    // QML 可调用的方法
    Q_INVOKABLE QString encryptPassword(const QString &password) const;

    // M8.2: 文件传输引擎（数据面 HTTP + 分片加解密 + 密文缓存）。
    // 经 main.cpp 注册为 QML context property "fileTransfer"，QML 直接连其
    // 信号与调用其方法；本类只负责它的控制面（90-99）与清单登记
    QObject *fileTransfer() const;
    Q_INVOKABLE QVariantList toVariantList(const QJsonArray &array) const;

signals:
    void loginSuccessful();
    void loginFailed(const QString &errorMessage);
    void registerSuccessful();
    void registerFailed(const QString &errorMessage);
    void logoutFinished();
    // P2: 会话失效（过期/被终止/续期被拒）——QML 据此回登录页并提示重新登录
    void sessionExpired();
    void connectionStateChanged(NetworkManager::ConnectionState state);
    void sessionChanged();
    // M3 信号
    void searchUsersResult(const QJsonArray &users);
    void contactsResult(const QJsonArray &contacts);
    void conversationsResult(const QJsonArray &conversations);
    void messageSent(qint64 messageId, qint64 conversationId, const QString &clientMessageId);
    void messageSendFailed(const QString &error);
    void newMessageReceived(const QJsonObject &message);
    void messagesSynced(qint64 conversationId, const QJsonArray &messages, bool hasMore);
    void messageAcked(qint64 messageId);
    // M5.5
    void messageStatusChanged(qint64 messageId, const QString &status);
    void eventsSynced(const QJsonArray &events, qint64 lastSeq, bool hasMore);
    // M9: 已读游标前进（多端已读同步）——QML 据此刷新当前会话消息已读态
    void readCursorAdvanced(qint64 conversationId, qint64 readMessageId);
    // M6: 对方身份公钥指纹变化（TOFU 告警，不阻塞发送）
    void peerIdentityChanged(qint64 peerUserId);
    // M7a: 群组操作结果与推送
    void groupCreated(qint64 conversationId, const QString &name);
    void groupMembersInvited(qint64 conversationId);
    void groupLeft(qint64 conversationId);
    void groupMemberKicked(qint64 conversationId, qint64 removedUserId);
    void groupInfoResult(const QJsonObject &info);
    void groupRequestFailed(const QString &error);
    void groupChanged(const QJsonObject &payload);
    // M9 特性栈：会话偏好与消息编辑/删除
    void conversationPrefsChanged(qint64 conversationId, bool pinned, bool muted);
    void messageEdited(qint64 conversationId, qint64 messageId,
                       const QString &content, const QString &editedAt);
    void messageDeleted(qint64 conversationId, qint64 messageId);
    void messageEditFailed(const QString &error);
    void messageDeleteFailed(const QString &error);
    // M10：收到会话成员的“正在输入”信号（服务端 fan-out）
    void typingReceived(qint64 conversationId, qint64 userId,
                        const QString &username, bool typing);
    // M10：会话删除结果（本端响应或其他成员/设备推送）
    void conversationDeleted(qint64 conversationId);
    void conversationDeleteFailed(const QString &error);

private slots:
    void onConnected();
    void onDisconnected();
    void onReadyRead();
    void onSocketError(QAbstractSocket::SocketError socketError);
    void onSslErrors(const QList<QSslError> &errors);
    void sendHeartbeat();

private:
    // M9 欠账修复：为客户端链路层单测开放私有处理器/状态（tests/unit/TestNetworkManager）
    friend class TestNetworkManager;

    void connectToServer();
    void initTls();
    void sendLoginRequest();
    void sendRegisterRequest();
    void handlePacket(const XYChat::Protocol::Packet &packet);
    void handleLoginResponse(const XYChat::Protocol::Packet &packet);
    void handleRegisterResponse(const XYChat::Protocol::Packet &packet);
    void handleLogoutResponse(const XYChat::Protocol::Packet &packet);
    void handleTokenRenewResponse(const XYChat::Protocol::Packet &packet);
    // P1: 服务端 MessageType::Error 回包（鉴权门/校验失败）：清理在途单发槽位并推进 healing 队列
    void handleErrorResponse(const XYChat::Protocol::Packet &packet);
    // P2: 会话续期与失效处理
    void scheduleTokenRenew();
    void notifySessionExpired();
    qint64 parseExpiresAt(const QString &iso) const;
    // M3 响应处理
    void handleSearchUsersResponse(const XYChat::Protocol::Packet &packet);
    void handleAddContactResponse(const XYChat::Protocol::Packet &packet);
    void handleGetContactsResponse(const XYChat::Protocol::Packet &packet);
    void handleGetConversationsResponse(const XYChat::Protocol::Packet &packet);
    void handleSendMessageResponse(const XYChat::Protocol::Packet &packet);
    void handleAckMessageResponse(const XYChat::Protocol::Packet &packet);
    void handleSyncMessagesResponse(const XYChat::Protocol::Packet &packet);
    void handleNewMessageNotification(const XYChat::Protocol::Packet &packet);
    // M5.5
    void handleMessageStatusUpdate(const XYChat::Protocol::Packet &packet);
    void handleSyncEventsResponse(const XYChat::Protocol::Packet &packet);
    // M9: 已读游标多端同步
    void handleReadCursorNotification(const XYChat::Protocol::Packet &packet);
    void applyReadCursor(qint64 conversationId, qint64 readMessageId);
    // M7a: 群组响应与推送
    void handleCreateGroupResponse(const XYChat::Protocol::Packet &packet);
    void handleInviteGroupMembersResponse(const XYChat::Protocol::Packet &packet);
    void handleLeaveGroupResponse(const XYChat::Protocol::Packet &packet);
    void handleKickGroupMemberResponse(const XYChat::Protocol::Packet &packet);
    void handleGetGroupInfoResponse(const XYChat::Protocol::Packet &packet);
    void handleGroupChangedNotification(const XYChat::Protocol::Packet &packet);
    // M9 特性栈：会话偏好与消息编辑/删除
    void handleSetConversationPrefsResponse(const XYChat::Protocol::Packet &packet);
    void handleConversationPrefsNotification(const XYChat::Protocol::Packet &packet);
    void handleEditMessageResponse(const XYChat::Protocol::Packet &packet);
    void handleDeleteMessageResponse(const XYChat::Protocol::Packet &packet);
    // M9 欠账修复：编辑/删除事件专用推送（88/89），与响应路径分离，不再靠 requestId==0 区分
    void handleMessageEditedNotification(const XYChat::Protocol::Packet &packet);
    void handleMessageDeletedNotification(const XYChat::Protocol::Packet &packet);
    // M10：“正在输入”推送
    void handleTypingNotification(const XYChat::Protocol::Packet &packet);
    // M10：会话整表删除（响应 + 推送）
    void handleDeleteConversationResponse(const XYChat::Protocol::Packet &packet);
    void handleConversationDeletedNotification(const XYChat::Protocol::Packet &packet);
    // M9 特性栈：会话偏好本地应用（响应/推送/事件共用）
    void applyConversationPrefs(qint64 conversationId, bool pinned, bool muted);
    quint64 sendEditMessageRequest(qint64 messageId, qint64 conversationId,
                                   const QString &content, const QString &contentType,
                                   const QString &plaintext);
    // M9 欠账修复：泵送等待 fetch_keys 的私聊编辑队列（fetch 单槽串行）
    void pumpPrivateEditFetch();
    // M7a: 群系统消息摘要（contentType=system 的结构化正文转可读文本）
    static QString systemMessageSummary(const QString &content);
    void sendPacket(const XYChat::Protocol::Packet &packet);
    quint64 nextRequestId();
    void setState(ConnectionState state);
    void resetAuthState();
    // M5: 重放保护辅助
    void addReplayProtection(QJsonObject &json);
    // M5.5: outbox 重发
    void flushOutbox();
    // M6: E2EE 引导与密钥交换
    void bootstrapE2ee();
    void sendRegisterKeysRequest();
    void sendFetchKeysRequest(qint64 toUserId);
    void handleRegisterKeysResponse(const XYChat::Protocol::Packet &packet);
    void handleFetchKeysResponse(const XYChat::Protocol::Packet &packet);
    // M7b: 群 E2EE 密钥包拉取
    void handleFetchGroupKeysResponse(const XYChat::Protocol::Packet &packet);
    // M6: 对指定用户加密正文（拉取的密钥包逐设备加密），失败返回空
    QString encryptForUser(qint64 toUserId, const QJsonArray &bundles, const QString &plaintext);
    // M6: 解密接收到的消息正文；非 envelope（存量明文）原样返回；
    // 解密失败返回空并置 undecryptable=true
    QString decryptIncomingContent(const QString &content, bool *undecryptable);
    // M6: 在接收 JSON 上就地解密 content 字段（含预览占位替换）
    void decryptMessageObject(QJsonObject &msg);
    // M9: 直接解密“编辑后”的新正文（绕过缓存、不预先清缓存）。成功时同步持久化
    // 解密缓存并返回 true；失败返回 false（由调用方回退保留既有可读正文）。
    // 与 decryptMessageObject 的关键区别：后者会命中缓存直接返回旧明文，编辑需强制重解新密文。
    bool decryptEditContent(const QJsonObject &data, QString &plaintext);
    // M7b: 群聊 E2EE  Sender Key 管理
    bool ensureGroupSenderKey(qint64 conversationId,
                              XYChat::Security::GroupE2eeCrypto::SenderKey &key);
    void sendFetchGroupKeysRequest(qint64 conversationId);
    QString buildGroupSenderKeyDistribution(qint64 conversationId,
                                            const QJsonObject &bundlesByUser);
    bool processGroupSenderKeyDistribution(const QJsonObject &msg);
    QString encryptGroupMessage(qint64 conversationId, const QString &plaintext);
    bool decryptGroupMessageObject(QJsonObject &msg);
    // P1-3: 群成员变更触发本端 Sender-Key 轮换与重分发（新成员获得密钥、
    // 被移除成员因轮换失去后续消息解密能力）；单发槽位占用时入队待推进
    void healGroupSenderKey(qint64 conversationId);
    void drainHealQueue(); // P1-3: 单发槽位空闲且就绪时从队列推进一个群的轮换重分发
    // M6.5: 本地持久化缓存
    // 登录后打开本地加密库：加载持久化 outbox、立即展示缓存会话、游标增量同步
    void openLocalStore();

    // M8.2: 文件传输。数据面（HTTP）由 FileTransferManager 负责，控制面（90-99）
    // 仍走本类的 TCP 通道：两者经"请求信号 + seq 回调"协作，因此传输引擎
    // 不持有 socket，可脱离网络单测
    void wireFileTransfer();
    void sendFileControlRequest(XYChat::Protocol::MessageType type, qint64 seq,
                                const QJsonObject &fields);
    void handleFileUploadCreateResponse(const XYChat::Protocol::Packet &packet);
    void handleFileUploadQueryResponse(const XYChat::Protocol::Packet &packet);
    void handleFileUploadCompleteResponse(const XYChat::Protocol::Packet &packet);
    void handleFileUploadCancelResponse(const XYChat::Protocol::Packet &packet);
    void handleFileDownloadTicketResponse(const XYChat::Protocol::Packet &packet);
    // 解密后的消息若为文件清单：登记到传输引擎（含密钥，仅 C++ 侧），
    // 并向消息对象补上脱敏展示字段（不含 key/iv）供 QML 渲染
    void attachFileInfo(QJsonObject &message);
    // emit 给 QML 前的唯一脱敏出口：登记清单（含密钥，留在 C++ 侧）、
    // 补脱敏展示字段，并把正文置空。清单含 32 字节文件密钥，字符串一旦
    // 进入 JS 引擎就无法可靠清零，因此每一条通向 UI 的消息都必须经此处。
    // 兼作兜底防线：即使 fileId 缺失或清单解析失败，只要正文形态像清单就置空
    void sanitizeForUi(QJsonObject &message);
    QJsonArray sanitizeArrayForUi(const QJsonArray &messages);
    void emitCachedConversations();
    // 将 sync_events 事件写入本地缓存并推进游标（hasMore 时自动续拉）
    void ingestSyncEvents(const QJsonArray &events, qint64 lastSeq, bool hasMore);

private:
    QSslSocket *m_sslSocket;
    QTimer *m_heartbeatTimer;
    QTimer *m_reconnectTimer;
    XYChat::Protocol::PacketCodec m_codec;
    ConnectionState m_state = ConnectionState::Disconnected;
    quint64 m_nextRequestId = 1;
    bool m_tlsEnabled = false;

    // 登录/注册待处理
    quint64 m_pendingLoginRequestId = 0;
    quint64 m_pendingRegisterRequestId = 0;
    QString m_pendingUsername;
    QString m_pendingPassword;
    QString m_pendingRegisterPassword;
    QString m_pendingEmail;
    QString m_pendingPhone;
    bool m_loginQueued = false;
    bool m_registerQueued = false;
    bool m_reconnectEnabled = false;

    // 认证后状态
    QString m_sessionToken;
    qint64 m_userId = 0;
    QString m_username;

    // P2: 会话续期与失效状态
    QTimer *m_tokenRenewTimer;        // 单发定时器：过期前触发自动续期
    qint64 m_sessionExpiresAtSecs = 0; // 会话过期时间（UTC epoch 秒；0 = 未知/未登录）
    quint64 m_pendingTokenRenewRequestId = 0;
    bool m_sessionExpiredNotified = false; // 已通知会话失效（幂等，避免重复弹窗）

    // M3: 待处理请求 ID
    quint64 m_pendingSearchRequestId = 0;
    quint64 m_pendingAddContactRequestId = 0;
    quint64 m_pendingGetContactsRequestId = 0;
    quint64 m_pendingGetConversationsRequestId = 0;
    quint64 m_pendingAckMessageRequestId = 0;
    quint64 m_pendingSyncMessagesRequestId = 0;

    // M5.5: TLS fail-closed 标记（CA 缺失且未显式允许明文时拒绝连接）
    bool m_tlsUnavailable = false;
    quint64 m_pendingSyncEventsRequestId = 0;

    // M7a: 群组请求待处理 ID
    quint64 m_pendingCreateGroupRequestId = 0;
    quint64 m_pendingInviteGroupRequestId = 0;
    quint64 m_pendingLeaveGroupRequestId = 0;
    quint64 m_pendingKickGroupRequestId = 0;
    quint64 m_pendingGetGroupInfoRequestId = 0;
    // M7b: 群 E2EE 引导状态
    quint64 m_pendingFetchGroupKeysRequestId = 0;
    qint64 m_fetchGroupKeysTargetConvId = 0;
    QSet<QString> m_pendingGroupDistributions; // clientMessageId 集合：等待 ACK 的分发消息
    QHash<qint64, XYChat::Security::GroupE2eeCrypto::SenderKey> m_groupSenderKeys; // 内存缓存
    QSet<qint64> m_healQueue; // P1-3: 待轮换重分发的群（单发槽位占用时排队）

    // M9 特性栈：会话偏好与消息编辑/删除
    quint64 m_pendingSetPrefsRequestId = 0;
    // M9 欠账修复：编辑/删除响应匹配改为多槽（镜像 m_pendingSendByRequestId），
    // 连续操作不再静默丢弃；私聊编辑经队列串行消费 fetch_keys 传输槽
    struct EditContext {
        qint64 messageId = 0;
        qint64 conversationId = 0;
        QString plaintext;   // 编辑后明文（本端乐观回填）
    };
    struct PrivateEditWait {
        qint64 messageId = 0;
        qint64 conversationId = 0;
        qint64 peerUserId = 0;
        QString plaintext;
    };
    QHash<quint64, EditContext> m_pendingEdits;   // 已发编辑请求：requestId → 上下文
    QQueue<PrivateEditWait> m_privateEditQueue;    // 等待 fetch_keys 的私聊编辑
    bool m_editFetchInFlight = false;              // 是否为编辑占用了 fetch_keys 传输槽
    QSet<quint64> m_pendingDeleteRequestIds;       // 已发删除请求的 requestId 集合

    // M10：“正在输入”客户端节流（conversationId -> 上次发送 epoch 毫秒）
    QHash<qint64, qint64> m_lastTypingSentMs;
    static constexpr qint64 TypingThrottleMs = 4000; // typing=true 每会话最快 4s 一次
    // M10：会话删除在途请求（requestId → conversationId）。多槽支持并发删除，
    // 对齐 M9 m_pendingEdits/m_pendingDeleteRequestIds；conversationId 以本地登记为准，
    // 即使响应 data 缺失也可靠（避免单值被后发请求覆盖导致首个删除静默丢失）
    QHash<quint64, qint64> m_pendingDeleteConversationRequests;

    // M5.5: 发送幂等与离线 outbox
    struct OutboxItem
    {
        QString clientMessageId;
        qint64 toUserId = 0;       // 私聊目标（群消息为 0）
        qint64 conversationId = 0; // M7a: 群聊目标会话（私聊为 0）
        QString content;
        // M8.2: 文件消息携带的 files.id（普通消息为 0）。仅存内存：
        // 持久化 outbox 表不带此列，落库后重发会退化成"正文是清单"的
        // 普通消息，等于把文件密钥当文本发给对方
        qint64 fileId = 0;
    };
    QList<OutboxItem> m_outbox;
    QHash<quint64, QString> m_pendingSendByRequestId; // requestId -> clientMessageId
    bool m_outboxFlushScheduled = false; // M11: 瞬时发送失败后已调度退避重刷，避免定时器堆叠

    // M6: E2EE 状态
    QString m_localDeviceId;                          // 登录时使用的 deviceId
    XYChat::Security::E2eeCrypto::KeyPair m_identityKey; // 本机身份密钥对
    QList<KeyStorage::PrekeyEntry> m_localPrekeys;    // 本地未消费的一次性预密钥
    bool m_e2eeReady = false;                         // 身份密钥已注册到服务端
    bool m_e2eeBootstrapPending = false;
    quint64 m_pendingRegisterKeysRequestId = 0;
    quint64 m_pendingFetchKeysRequestId = 0;
    qint64 m_fetchKeysTargetUserId = 0;               // 在途 FetchKeys 的目标用户
    QHash<qint64, qint64> m_fetchBackoffUntil;        // userId -> 重试等待截止时间（秒），避免对未注册密钥的目标空转拉取
    int m_serverPrekeyRemaining = -1;                 // 服务端报告的未认领预密钥余量
    QHash<qint64, QString> m_decryptCache;            // messageId -> 已解密正文（避免重复消费预密钥）
    bool m_decryptCacheLoaded = false;                // 本次登录是否已从磁盘加载解密缓存

    // M6.5: 本地加密持久化缓存（会话/消息/outbox/解密缓存/同步游标）
    LocalStore m_localStore;

    // M8.2: 文件传输引擎（本类拥有，并经 main.cpp 注册为 QML context property）
    XYChat::Client::FileTransferManager *m_fileTransfer = nullptr;
    // TCP requestId -> 传输引擎的 seq（响应到达时反查，用后即删）
    QHash<qint64, qint64> m_fileSeqByRequestId;
};
