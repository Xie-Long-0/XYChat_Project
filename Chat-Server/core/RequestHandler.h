#pragma once

#include <QThread>
#include <QSslSocket>
#include <QSslConfiguration>
#include <QJsonObject>
#include <QTimer>
#include <QElapsedTimer>

#include <atomic>

#include "protocol/PacketCodec.h"
#include "RateWindow.h"

class DatabaseManager;
class NonceCache;
struct FileRecord;
namespace XYChat::Server { class IObjectStorage; }

class RequestHandler : public QThread
{
    Q_OBJECT

public:
    explicit RequestHandler(qintptr socketDescriptor, QObject *parent = nullptr);
    ~RequestHandler() override;

    // 供 Server 查询当前认证用户
    qint64 authenticatedUserId() const { return m_authenticatedUserId; }
    qint64 currentSessionId() const { return m_currentSessionId; }

    // M3: 供 Server 转发数据到客户端
    void sendRawData(const QByteArray &data);

    // M5.5: 请求断开客户端连接（被 terminate_session 时由 Server 调用）
    void disconnectClient();

    // M5: 设置 TLS 配置（由 Server 在 start() 前调用）
    void setSslConfiguration(const QSslConfiguration &config);

    // M5.5: 设置全局 nonce 缓存（由 Server 在 start() 前调用）
    void setNonceCache(NonceCache *cache);

    // M8: 设置对象存储（由 Server 在 start() 前调用，存储初始化成功才会注入）。
    // 为空时所有文件请求一律回 FileStorageFailed，不让文件消息静默退化成文本消息
    void setObjectStorage(XYChat::Server::IObjectStorage *storage);

    // M8.2: 文件传输数据面（HTTP(S)）的基地址，随登录响应下发。
    // 为空表示数据面未启动，此时不下发该字段，客户端据此禁用文件能力
    // （而不是自行猜端口，否则部署拓扑一变就要重发客户端）
    void setFileTransferBaseUrl(const QString &url);

signals:
    void finished();
    void userLoggedIn(qint64 userId, qint64 sessionId, const QString &deviceId);
    void userLoggedOut(qint64 userId, qint64 sessionId);
    // M3: 消息路由信号
    void messageForUser(qint64 targetUserId, const QByteArray &packetData);
    // M5.5: 本人其他会话被终止，需断开对应连接
    void sessionTerminated(qint64 sessionId);

private slots:
    void onReadyRead();
    void onIdleTimeout();

private:
    void run() override;

    void processPacket(const XYChat::Protocol::Packet &packet);

    // 各请求处理器
    void processLoginRequest(const XYChat::Protocol::Packet &packet, const QJsonObject &request);
    void processRegisterRequest(const XYChat::Protocol::Packet &packet, const QJsonObject &request);
    void processLogoutRequest(const XYChat::Protocol::Packet &packet);
    void processTokenRenewRequest(const XYChat::Protocol::Packet &packet, const QJsonObject &request);
    // M5.5: 原 force_logout 改为仅允许终止本人其他会话
    void processTerminateSessionRequest(const XYChat::Protocol::Packet &packet, const QJsonObject &request);

    // M3 处理器
    void processSearchUsersRequest(const XYChat::Protocol::Packet &packet, const QJsonObject &request);
    void processAddContactRequest(const XYChat::Protocol::Packet &packet, const QJsonObject &request);
    void processGetContactsRequest(const XYChat::Protocol::Packet &packet);
    void processGetConversationsRequest(const XYChat::Protocol::Packet &packet);
    void processSendMessageRequest(const XYChat::Protocol::Packet &packet, const QJsonObject &request);
    void processAckMessageRequest(const XYChat::Protocol::Packet &packet, const QJsonObject &request);
    void processSyncMessagesRequest(const XYChat::Protocol::Packet &packet, const QJsonObject &request);
    // M5.5: 账号级增量同步
    void processSyncEventsRequest(const XYChat::Protocol::Packet &packet, const QJsonObject &request);
    // M6: 端到端加密密钥注册与拉取
    void processRegisterKeysRequest(const XYChat::Protocol::Packet &packet, const QJsonObject &request);
    void processFetchKeysRequest(const XYChat::Protocol::Packet &packet, const QJsonObject &request);
    // M7a: 明文群聊处理器
    void processCreateGroupRequest(const XYChat::Protocol::Packet &packet, const QJsonObject &request);
    void processInviteGroupMembersRequest(const XYChat::Protocol::Packet &packet, const QJsonObject &request);
    void processLeaveGroupRequest(const XYChat::Protocol::Packet &packet, const QJsonObject &request);
    void processKickGroupMemberRequest(const XYChat::Protocol::Packet &packet, const QJsonObject &request);
    void processGetGroupInfoRequest(const XYChat::Protocol::Packet &packet, const QJsonObject &request);
    // M7b: 拉取群内所有成员的 E2EE 密钥包（供 Sender Key 分发）
    void processFetchGroupKeysRequest(const XYChat::Protocol::Packet &packet, const QJsonObject &request);
    // M9 特性栈：会话置顶/免打扰与消息编辑/删除
    void processSetConversationPrefsRequest(const XYChat::Protocol::Packet &packet, const QJsonObject &request);
    void processEditMessageRequest(const XYChat::Protocol::Packet &packet, const QJsonObject &request);
    void processDeleteMessageRequest(const XYChat::Protocol::Packet &packet, const QJsonObject &request);

    // M8: 媒体、文件与对象存储的控制面处理器（数据面走独立 HTTP(S) 服务，不在此处）。
    // 控制面以会话 token 鉴权 + 上传者归属校验；票据只给无会话的 HTTP 数据面使用
    void processFileUploadCreateRequest(const XYChat::Protocol::Packet &packet, const QJsonObject &request);
    void processFileUploadQueryRequest(const XYChat::Protocol::Packet &packet, const QJsonObject &request);
    void processFileUploadCompleteRequest(const XYChat::Protocol::Packet &packet, const QJsonObject &request);
    void processFileUploadCancelRequest(const XYChat::Protocol::Packet &packet, const QJsonObject &request);
    void processFileDownloadTicketRequest(const XYChat::Protocol::Packet &packet, const QJsonObject &request);

    // M8: 校验 send_message 携带的 fileId。返回 Ok 时 *outFileId 为可入库的文件 ID
    // （未携带文件则为 0）；否则返回应回给客户端的错误码，outReason 为文案
    XYChat::Protocol::ErrorCode checkMessageFile(qint64 fileId, qint64 *outFileId,
                                                 QString *outReason);
    // M8: 载入文件记录并校验调用者为上传者（上传控制面共用前置检查）。
    // 返回非 Ok 时 *outReason 为可直接回给客户端的文案
    XYChat::Protocol::ErrorCode requireOwnedFile(qint64 fileId, FileRecord *outRecord,
                                                 QString *outReason);

    // M7a: 群消息发送（明文入库 + fan-out，与私聊 E2EE 路径分流）
    void processSendGroupMessage(const XYChat::Protocol::Packet &packet, const QJsonObject &request,
                                 qint64 conversationId, const QString &clientMessageId);
    // M7a: 群系统消息入库并 fan-out（contentType=system，无幂等键）
    void postGroupSystemMessage(qint64 conversationId, qint64 operatorId, const QJsonObject &payload);
    // M7a: 向全体现任成员推送群变更通知并写入各自 sync_events
    void notifyGroupChanged(qint64 conversationId, const QString &changeType,
                            qint64 operatorId, qint64 targetUserId);

    // M5.5: 重放保护（timestamp/nonce 强制必填）
    bool checkReplayProtection(const QJsonObject &request);

    // 工具方法
    void sendResponse(quint64 requestId,
                      XYChat::Protocol::MessageType messageType,
                      XYChat::Protocol::ErrorCode code,
                      const QString &message,
                      const QJsonObject &data = {});
    void sendPacket(const XYChat::Protocol::Packet &packet);
    bool validateSession(const QJsonObject &request);
    bool checkRateLimit(const QString &ipAddress, qint64 userId);

private:
    QSslSocket *m_socket = nullptr;
    QTimer *m_idleTimer = nullptr;
    XYChat::Protocol::PacketCodec m_codec;
    qintptr m_socketDescriptor;

    // M5: TLS
    QSslConfiguration m_sslConfig;
    bool m_tlsEnabled = false;

    // M5.5: 全局 nonce 缓存（Server 持有，各连接共享）
    NonceCache *m_nonceCache = nullptr;

    // M8: 对象存储（Server 持有，各连接共享）；为空表示存储不可用
    XYChat::Server::IObjectStorage *m_objectStorage = nullptr;

    // M8.2: 数据面基地址（Server 注入，例 https://host:12346/file）
    QString m_fileTransferBaseUrl;

    // M5.5: 发送代理对象，线程亲和于 handler 线程，
    // 避免跨线程直接访问 QSslSocket
    std::atomic<QObject *> m_sendWorker{nullptr};

    // 认证状态
    qint64 m_authenticatedUserId = 0;
    qint64 m_currentSessionId = 0;
    QString m_currentDeviceId;

    // M6/M11: 连接级限流窗口（fetch_keys 与 fetch_group_keys 共享一个窗口）
    XYChat::Server::RateWindow m_fetchKeysWindow;
    // M11 前置: 发消息与搜索限流（连接级固定窗口，防刷消息/用户名枚举）
    XYChat::Server::RateWindow m_sendWindow;
    XYChat::Server::RateWindow m_searchWindow;
    // M9 欠账修复: 编辑/删除共用窗口与偏好设置窗口（每次调用按成员数写 sync_events
    // + fan-out，O(N) 放大且事件 30 天才清理，需与 send/search 一致限流防刷库）
    XYChat::Server::RateWindow m_editDeleteWindow;
    XYChat::Server::RateWindow m_prefsWindow;
    // M8: 文件控制面限流。新建上传会分配磁盘与 DB 行，配额更紧；
    // 查询/完成/取消/下载票据为廉价读写，共用一个较宽窗口
    XYChat::Server::RateWindow m_fileUploadWindow;
    XYChat::Server::RateWindow m_fileOpsWindow;

    // M11 前置: 结构化日志的每请求上下文（起始计时/请求类型/请求 ID）
    QElapsedTimer m_requestTimer;
    QString m_currentRequestType;
    quint64 m_currentRequestId = 0;

    // 数据库（每个线程使用独立连接名）
    DatabaseManager *m_db = nullptr;

    // 限流常量
    static constexpr int MaxFailedLoginsPerIP = 10;
    static constexpr int MaxFailedLoginsPerUser = 5;
    static constexpr int RateLimitWindowSeconds = 300; // 5 分钟
    static constexpr int ReplayTimestampToleranceSecs = 300; // 5 分钟时间戳容差

    // M6: 预密钥上传限制
    static constexpr int MaxPrekeysPerBatch = 100;   // 单批上传上限
    static constexpr int MaxPrekeysPerDevice = 500;  // 每设备未认领预密钥总量上限
    // M6 审查修复：fetch_keys 频率限制，防止恶意耗尽他人预密钥池
    static constexpr int MaxFetchKeysPerWindow = 20;  // 窗口内拉取上限
    static constexpr int FetchKeysWindowSeconds = 60; // 滑动窗口长度

    // M11 前置：发消息限流（连接级固定窗口，覆盖私聊/群聊 send_message）
    static constexpr int MaxSendMessagesPerWindow = 30; // 窗口内发消息上限
    static constexpr int SendMessageWindowSeconds = 10; // 窗口长度（秒）
    // M11 前置：用户搜索限流（连接级固定窗口，抑制用户名枚举/刷库）
    static constexpr int MaxSearchesPerWindow = 20; // 窗口内搜索上限
    static constexpr int SearchWindowSeconds = 60;  // 窗口长度（秒）
    // M9 欠账修复：编辑/删除共用限流（连接级固定窗口，抑制刷库/O(N) 事件放大）
    static constexpr int MaxEditDeletePerWindow = 20; // 窗口内编辑+删除总上限
    static constexpr int EditDeleteWindowSeconds = 60; // 窗口长度（秒）
    // M9 欠账修复：会话偏好设置限流（置顶/免打扰高频切换无意义，放宽上限）
    static constexpr int MaxPrefsPerWindow = 30;      // 窗口内偏好设置上限
    static constexpr int PrefsWindowSeconds = 60;     // 窗口长度（秒）

    // M7a: 群消息明文长度上限（单条 UTF-8 字符数；M7b E2EE / M8 媒体另行调整）
    static constexpr int MaxGroupMessageLength = 16384;
    // M7a: 群名长度上限（字符数）
    static constexpr int MaxGroupNameLength = 64;

    // M8: 文件控制面限流（连接级固定窗口）。新建上传会占用磁盘与元数据行，
    // 且超期未完成的文件需等回收任务清理，因此比查询类操作收得更紧
    static constexpr int MaxFileUploadsPerWindow = 20; // 窗口内新建上传数上限
    static constexpr int FileUploadWindowSeconds = 60; // 窗口长度（秒）
    static constexpr int MaxFileOpsPerWindow = 60;     // 查询/完成/取消/下载票据总上限
    static constexpr int FileOpsWindowSeconds = 60;    // 窗口长度（秒）
};
