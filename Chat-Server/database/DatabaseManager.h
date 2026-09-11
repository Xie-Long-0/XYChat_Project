#pragma once

#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <QSqlDatabase>

#include <optional>

// 数据结构
struct UserInfo
{
    qint64 id = 0;
    QString username;
    QString email;
    QString phone;
    QString passwordHash;
    QString createdAt;
    QString updatedAt;
};

struct SessionInfo
{
    qint64 id = 0;
    qint64 userId = 0;
    QString deviceId;
    QString tokenHash;
    QString loginIp;
    QString createdAt;
    QString lastActiveAt;
    QString expiresAt;
};

struct ContactInfo
{
    qint64 id = 0;
    qint64 userId = 0;
    qint64 contactUserId = 0;
    QString contactUsername;
    QString createdAt;
};

struct ConversationInfo
{
    qint64 id = 0;
    QString type; // "private" / "group"
    QString createdAt;
    QString updatedAt;
    // 对于一对一会话，记录对方信息
    qint64 peerUserId = 0;
    QString peerUsername;
    QString lastMessage;
    qint64 lastMessageId = 0;
    QString lastMessageAt;
    int unreadCount = 0;
    // M7a: 群聊会话信息（private 会话 name 为空、memberCount 为 0）
    QString name;
    int memberCount = 0;
    // M9 特性栈：会话偏好（按成员×会话维度，服务端权威；客户端缓存）
    bool pinned = false;
    bool muted = false;
};

struct MessageInfo
{
    qint64 id = 0;
    qint64 conversationId = 0;
    qint64 senderId = 0;
    QString senderUsername;
    QString content;
    QString contentType; // "text"
    QString status;      // "sending", "sent", "delivered", "read", "failed"
    QString createdAt;
    QString clientMessageId; // M5.5: 客户端幂等键
    // M9 特性栈：消息编辑/删除（软删除留墓碑，正文清空）
    QString editedAt;   // 非空表示已编辑（编辑时间）
    bool deleted = false;
    // M8: 关联的文件 ID（>0 表示这是一条文件消息，正文密文解出后为 FileManifest）
    qint64 fileId = 0;
};

// M8: 文件元数据
// 服务端只见客户端加密后的密文，因此本结构不含文件名/MIME/明文大小等敏感
// 元数据（它们只在 FileManifest 中随消息正文 E2EE 传输）
struct FileRecord
{
    qint64 id = 0;
    QString blobKey;          // 对象存储键（服务端生成，不含用户可控成分）
    qint64 uploaderId = 0;
    QString uploaderDeviceId;
    qint64 sizeBytes = 0;     // 密文总字节数
    qint64 chunkSize = 0;     // 密文分片大小（含 16 字节 GCM 标签）
    int chunkCount = 0;       // 分片数
    QString sha256Hex;        // 密文整体 SHA-256（小写 hex）
    QString status;           // "uploading" / "ready" / "cancelled" / "failed"
    QString createdAt;
    QString completedAt;
};

// M8: 文件票据（上传/下载授权凭据）
// 服务端只存票据的 SHA-256 摘要，明文仅在签发响应中返回一次
// （与 session token 同一套做法）
struct FileTicketInfo
{
    qint64 id = 0;
    qint64 fileId = 0;
    qint64 userId = 0;
    QString kind;       // "upload" / "download"
    QString expiresAt;
    QString createdAt;
    bool used = false;  // 已消费（一次性票据用毕置位）
};

// M5.5: 账号级同步事件
struct SyncEventInfo
{
    qint64 seq = 0;
    qint64 userId = 0;
    QString eventType; // "message", "contact_added", "receipt"
    QString payload;   // JSON
    QString createdAt;
};

// M6: 端到端加密密钥
struct DeviceIdentityKey
{
    QString deviceId;
    QString identityPub; // Base64 编码的 X25519 公钥
};

// M6: 认领后的一次性预密钥（每设备一个）
struct ClaimedPrekey
{
    QString deviceId;
    qint64 prekeyId = 0;
    QString prekeyPub; // Base64 编码的 X25519 公钥
};

// DatabaseManager
class DatabaseManager
{
public:
    explicit DatabaseManager(const QString &connectionName = "main");
    ~DatabaseManager();

    // 初始化：打开连接并运行迁移
    bool initialize();

    // 用户管理
    bool userExists(const QString &username);
    std::optional<UserInfo> getUserByUsername(const QString &username);
    // M7a: 按 ID 查询用户名（不存在返回空串）
    QString usernameById(qint64 userId);
    qint64 registerUser(const QString &username,
                        const QString &email,
                        const QString &phone,
                        const QString &passwordHash);

    // Session 管理
    qint64 createSession(qint64 userId,
                         const QString &deviceId,
                         const QString &tokenHash,
                         const QString &loginIp,
                         int ttlSeconds = 86400 * 7); // 7 天
    std::optional<SessionInfo> getSessionByTokenHash(const QString &tokenHash);
    std::optional<SessionInfo> getSessionById(qint64 sessionId); // M5.5: 续期时校验 token
    bool updateSessionLastActive(qint64 sessionId);
    bool deleteSession(qint64 sessionId);
    bool deleteSessionsByUserId(qint64 userId);
    QList<SessionInfo> getSessionsByUserId(qint64 userId);

    // 登录审计
    void recordLoginAttempt(qint64 userId,
                            const QString &ipAddress,
                            bool success,
                            const QString &failureReason = {});
    int recentFailedLoginCount(const QString &ipAddress, int windowSeconds = 300);
    int recentFailedLoginCountForUser(qint64 userId, int windowSeconds = 300);

    // 设备管理
    bool registerDevice(qint64 userId,
                        const QString &deviceId,
                        const QString &deviceName,
                        const QString &platform);
    QList<QJsonObject> getDevicesByUserId(qint64 userId);
    bool removeDevice(qint64 userId, const QString &deviceId);

    // 用户搜索
    QList<UserInfo> searchUsers(const QString &query, int limit = 20);

    // 联系人管理
    bool addContact(qint64 userId, qint64 contactUserId);
    bool removeContact(qint64 userId, qint64 contactUserId);
    QList<ContactInfo> getContacts(qint64 userId);
    bool isContact(qint64 userId, qint64 contactUserId);

    // 会话管理
    qint64 getOrCreatePrivateConversation(qint64 userId1, qint64 userId2);
    QList<ConversationInfo> getConversationsForUser(qint64 userId);
    std::optional<ConversationInfo> getConversation(qint64 conversationId);
    // M5.5: 授权检查（先授权再查询）
    bool isConversationMember(qint64 conversationId, qint64 userId);
    bool canAccessMessage(qint64 messageId, qint64 userId);

    // 消息管理
    //
    // fileId > 0 时，“该文件仍处于 ready”这一条件与 INSERT 合并为单条语句：
    // 维护任务会把“已就绪但无引用”的文件原子迁入终态，若校验在语句之外，
    // 本连接读到 ready 之后、写入之前该行可能被迁移，产出一条指向已取消
    // 文件的消息（其磁盘数据随后被回收 → 附件永久打不开）。单语句在 SQLite 内
    // 原子且写者互相串行，因此两种交错都安全：要么本条消息先落库（迁移语句的
    // NOT EXISTS 检测到引用而放弃），要么迁移先提交（本语句查不到 ready 行而不写入）。
    // 未写入时置 *fileNotReady 并返回 -1，调用方据此回 FileNotReady 而非误导性的
    // InternalError（镜像 createFileRecord 的 quotaExceeded 口径）
    qint64 sendMessage(qint64 conversationId, qint64 senderId,
                       const QString &content, const QString &contentType = "text",
                       const QString &clientMessageId = {},
                       const QString &senderDeviceId = {},
                       qint64 fileId = 0, bool *fileNotReady = nullptr);
    std::optional<MessageInfo> getMessage(qint64 messageId);
    // M5.5: 客户端幂等键去重
    std::optional<MessageInfo> getMessageByClientKey(qint64 senderId,
                                                     const QString &senderDeviceId,
                                                     const QString &clientMessageId);
    QList<MessageInfo> getMessages(qint64 conversationId, qint64 beforeId = 0, int limit = 50);
    QList<MessageInfo> syncMessages(qint64 conversationId, qint64 afterId, int limit = 100);
    bool updateMessageStatus(qint64 messageId, const QString &status);
    bool updateMessagesReadStatus(qint64 conversationId, qint64 readerId);
    int getUnreadCount(qint64 conversationId, qint64 userId);
    // M9 特性栈：消息编辑（覆盖正文并标记编辑时间；仅发送者调用，调用方已授权）与删除（软删除留墓碑）
    bool editMessage(qint64 messageId, const QString &content, const QString &contentType);
    bool deleteMessage(qint64 messageId);

    // M5.5: 消息回执（per-recipient，替代全局状态聚合）
    bool recordMessageReceipt(qint64 messageId, qint64 userId,
                              const QString &deviceId, const QString &status);
    int receiptCount(qint64 messageId, const QString &status); // "delivered" / "read"
    // M7a: 按接收用户去重的回执计数（多设备不重复计数），供送达/已读人数聚合
    int receiptUserCount(qint64 messageId, const QString &status);
    bool updateMemberReadCursor(qint64 conversationId, qint64 userId, qint64 messageId);

    // M5.5: 同步事件流
    qint64 appendSyncEvent(qint64 userId, const QString &eventType, const QString &payloadJson);
    QList<SyncEventInfo> getSyncEvents(qint64 userId, qint64 afterSeq, int limit = 200);
    // M9: sync_events 保留清理与落后检测
    int pruneSyncEvents(int retentionDays);  // 删除早于保留期的事件，返回删除数
    qint64 prunedBelowSeq();                 // 清理水位线：该 seq 及以下事件已不可用
    qint64 maxSyncEventSeq();                // 当前全局最大 seq（供全量回退重置游标）

    // M6: 端到端加密密钥管理
    // 注册/更新设备身份公钥（仅公钥，私钥永不离开客户端）
    bool upsertIdentityKey(qint64 userId, const QString &deviceId, const QString &identityPub);
    QList<DeviceIdentityKey> getIdentityKeysByUser(qint64 userId);

    // 批量上传一次性预密钥公钥，返回上传数量（失败返回 -1）
    int uploadPrekeys(qint64 userId, const QString &deviceId, const QStringList &prekeyPubs);
    // 设备剩余未认领预密钥数量
    int prekeyCount(qint64 userId, const QString &deviceId);
    // 事务内为指定用户每个有库存的设备原子认领一个 unused 预密钥
    QList<ClaimedPrekey> claimPrekeys(qint64 userId);
    // 校验 envelope 中的预密钥属于接收方且处于 claimed 状态
    bool validateClaimedPrekey(qint64 userId, const QString &deviceId, qint64 prekeyId);
    // 消息入库后消费预密钥（claimed -> used），返回实际消费的条数
    int consumePrekeys(const QList<qint64> &prekeyIds);
    // 删除设备全部密钥材料（身份密钥 + 预密钥）
    bool removeDeviceKeys(qint64 userId, const QString &deviceId);

    // M7a: 群组管理（仅数据访问，成员存在性与权限校验由调用方负责）
    // 事务内创建 group 会话：写入创建者（role=owner）与初始成员（role=member，
    // 自动去重、剔除创建者自身、按成员上限截断）；失败返回 -1
    qint64 createGroup(qint64 ownerId, const QString &name, const QList<qint64> &memberIds);
    // 批量加入成员，已在群中的用户跳过；数据库错误返回 false
    bool addGroupMembers(qint64 conversationId, const QList<qint64> &userIds);
    bool removeGroupMember(qint64 conversationId, qint64 userId);
    bool updateMemberRole(qint64 conversationId, qint64 userId, const QString &role);
    // 成员角色："owner" / "admin" / "member"，非成员返回空串
    QString groupRole(qint64 conversationId, qint64 userId);
    QList<QJsonObject> getGroupMembers(qint64 conversationId);
    bool setGroupName(qint64 conversationId, const QString &name);
    // M7a: 群成员 ID 列表（按入群顺序，供消息 fan-out 与群变更通知）
    QList<qint64> getGroupMemberIds(qint64 conversationId);
    // M9 特性栈：会话成员 ID 列表（私聊/群聊通用，供编辑/删除 fan-out）
    QList<qint64> getConversationMemberIds(qint64 conversationId);
    // M7a: 会话成员数（排除指定用户，供回执聚合计算接收者总数）
    int memberCountExcluding(qint64 conversationId, qint64 excludeUserId);

    // M9 特性栈：会话偏好（置顶/免打扰，按成员×会话维度）
    bool setConversationPrefs(qint64 conversationId, qint64 userId,
                              bool pinned, bool muted);
    // 读取成员会话偏好（非成员返回默认 false/false）
    std::optional<std::pair<bool, bool>> getConversationPrefs(qint64 conversationId,
                                                              qint64 userId);

    // M7a: 群组规模约束（供数据层与业务层统一引用）
    static constexpr int MaxGroupMembers = 200;  // 单群成员上限
    static constexpr int MaxInviteBatch = 100;   // 单次邀请批量上限

    // M8: 文件元数据（对象存储的数据面独立，本层只管元数据、票据与消息关联）
    // 创建一条 uploading 状态的文件记录，返回 files.id（失败返回 -1）。
    // blobKey 由对象存储分配，本层只负责持久化，不校验磁盘状态。
    // maxConcurrentUploads > 0 时在单条 INSERT...SELECT 内原子校验并发配额：
    // 先读计数再插入的 TOCTOU 会让多连接并发创建全部读到"未满"而集体放行，
    // 使软配额形同虚设。配额已满时置 *quotaExceeded 并返回 -1，以便调用方
    // 回 FileQuotaExceeded 而不是误导性的 InternalError
    qint64 createFileRecord(qint64 uploaderId, const QString &uploaderDeviceId,
                            const QString &blobKey, qint64 sizeBytes, qint64 chunkSize,
                            int chunkCount, const QString &sha256Hex,
                            int maxConcurrentUploads = 0, bool *quotaExceeded = nullptr);
    std::optional<FileRecord> getFileRecord(qint64 fileId);
    // 状态迁移：仅允许 uploading -> ready/cancelled/failed，终态不可再改。
    // 记录不存在、已处终态或写入失败均返回 false，调用方需先读状态实现幂等
    // （重发完成请求时若已为 ready 则直接回成功，不重复迁移）
    bool markFileReady(qint64 fileId);
    bool markFileCancelled(qint64 fileId);
    bool markFileFailed(qint64 fileId);
    // 该用户处于 uploading 状态的文件数（观测/诊断用；查询失败返回 -1，
    // 调用方据此区分"确无在传文件"与"用量不明"）。
    // 配额强制不靠本函数：先读计数再插入存在 TOCTOU，强制在 createFileRecord
    // 内以单条语句原子完成
    int uploadingCountForUser(qint64 userId);
    // 取超期未完成的上传（status=uploading 且 created_at 早于 staleHours 小时前），
    // 供回收任务清理磁盘与数据库行。时间比较交给 SQLite datetime('now')，
    // 避开调用方与列默认值两种时间格式字典序不一致的陷阱
    QList<FileRecord> getStaleUploads(int staleHours, int limit = 100);
    // 取处于终态（cancelled/failed）且创建时间早于 olderThanHours 小时前的记录，
    // 供回收任务收尾：幂等清磁盘（覆盖"先落状态后删盘失败"留下的孤儿）
    // 后删元数据行，避免 files 表无界增长
    QList<FileRecord> getTerminalFiles(int olderThanHours, int limit = 100);
    // 取已就绪但已无任何未删除消息引用的记录（完成时间早于 olderThanHours
    // 小时前），供回收任务删磁盘与元数据行。两个来源：附件所在消息被软删除
    //（删除后客户端已无下载路径），以及上传完成但发送始终未发生。不清理则
    // 每条被删消息都永久占用一份存储
    //
    // 时限以 completed_at 为基准（缺失时退回 created_at）：大文件的续传可能
    // 跨越数天，若按 created_at 算，刚完成的上传会被立即当成孤儿删掉
    QList<FileRecord> getUnreferencedReadyFiles(int olderThanHours, int limit = 100);
    // 文件是否已被某条未删除的消息引用（引用中的文件不得回收）
    bool isFileReferencedByMessage(qint64 fileId);
    // 该用户是否可访问某文件：本人上传，或其所属会话中存在未删除且引用该文件
    // 的消息。下载票据签发前必须过这一关，否则任何人可凭 fileId 枚举下载他人文件
    bool canUserAccessFile(qint64 fileId, qint64 userId);
    // 删除文件元数据行（仅限未被消息引用的记录，供回收任务调用）。
    // 行不存在也返回 true（幂等），仅 SQL 错误返回 false
    bool deleteFileRecord(qint64 fileId);
    // 把"已就绪但已无未删除消息引用"的行原子地转为 cancelled，返回是否由本次
    // 调用完成迁移（已被引用、状态不是 ready、行不存在或 SQL 失败均返回 false）。
    // 引用判定与迁移必须合并为单条语句：分步版本存在 TOCTOU，并发 sendMessage
    // 可能在两步之间引用该文件，随后磁盘数据被回收任务删掉，留下一条指向
    // 空数据的消息（用户侧表现为附件永久打不开）
    //
    // 本函数只保证“迁移那一刻无引用”。迁移之后仍可能有消息引用该文件：
    // 发送侧的文件校验与消息写入存在跨线程窗口。因此两侧都做了防护：
    // sendMessage 将“文件仍为 ready”下推为 INSERT 的守卫子查询（单语句原子，
    // 写者串行，两种交错都安全），终态回收那一轮在删盘前再判一次引用
    bool cancelUnreferencedReadyFile(qint64 fileId);

    // M8: 文件票据（服务端只存 SHA-256 摘要，明文票据仅签发时返回一次）
    // 过期时间由 SQLite 按 ttlSeconds 计算并写入，与校验时同一时钟源
    bool issueFileTicket(qint64 fileId, qint64 userId, const QString &kind,
                         const QString &ticketHash, int ttlSeconds);
    // 校验票据是否可用于指定用途：存在、kind 匹配、未过期、未消费。
    // 四项条件全部下推到 SQL，不通过时返回 nullopt（调用方回 InvalidFileTicket，
    // 不区分具体原因以免被用来探测票据库）
    std::optional<FileTicketInfo> validateFileTicket(const QString &ticketHash,
                                                     const QString &kind);
    // 标记票据已消费（一次性票据用毕置位，重复使用即失效）
    bool markFileTicketUsed(qint64 ticketId);
    // 吊销某文件某类型的全部票据，返回删除条数（入参非法或 SQL 错误返回 -1）。
    // 上传完成或取消后立即调用：票据已无用途，而上传票据 TTL 长达 24 小时，
    // 留着只会白白延长泄露窗口（分片在完成后已被组装回收，持票也无处可用）
    int revokeFileTickets(qint64 fileId, const QString &kind);
    // 清理已过期票据，返回删除条数（SQL 错误返回 -1）
    int pruneExpiredFileTickets();

    // 手动事务包装（供调用方将多个写操作绑定为原子单元）
    bool beginTransaction();
    bool commitTransaction();
    bool rollbackTransaction();

private:
    bool openDatabase();
    void closeDatabase();
    bool runMigrations();
    bool migrateToV1();
    bool migrateToV2();
    bool migrateToV3();
    bool migrateToV4();
    bool migrateToV5();
    bool migrateToV6();
    bool migrateToV7();
    bool migrateToV8();
    bool migrateToV9();
    bool migrateToV10();

    // M7a: 插入单个会话成员（供 createGroup/addGroupMembers 复用）
    bool insertMember(qint64 conversationId, qint64 userId, const QString &role);

    QString m_connectionName;
};
