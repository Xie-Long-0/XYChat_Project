#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>
#include <QSqlDatabase>

/**
 * M6.5: 客户端本地持久化缓存（按账号 + 设备隔离）
 *
 * - SQLite 本地数据库：AppDataLocation/localstore/<username>_<deviceId>.db
 * - 消息正文与会话预览以 AES-256-GCM 加密后落库（存储密钥随机生成，
 *   经 KeyStorage DPAPI 保护），磁盘上不存在可读的消息明文
 * - 包含持久化 outbox、解密缓存（归口替代 M6 的 KeyStorage .cache 文件）
 *   与 sync_events 同步游标
 * - 登出时经 closeAndDestroy() 整体删除（E2EE 身份密钥不在此处，仍由
 *   KeyStorage 保留以便下次登录复用）
 */
class LocalStore
{
public:
    struct OutboxItem
    {
        QString clientMessageId;
        qint64 toUserId = 0;      // 私聊目标（群消息为 0）
        qint64 conversationId = 0; // M7a: 群聊目标会话（私聊为 0）
        QString content; // 明文正文（群消息为原文，私聊为待加密明文）
    };

    LocalStore() = default;
    ~LocalStore();
    LocalStore(const LocalStore &) = delete;
    LocalStore &operator=(const LocalStore &) = delete;

    // 打开/关闭。打开时加载（或首次生成）存储密钥并建表；
    // 密钥无法持久化时返回 false（fail-closed：宁可无缓存也不落明文）
    bool open(const QString &username, const QString &deviceId);
    // 仅关闭数据库（保留文件，供下次启动复用）
    void close();
    // 清除用户可见数据（消息/会话/outbox/同步游标），但保留解密缓存与
    // 存储密钥：二者属 E2EE 密钥材料：一次性预密钥消费后不可恢复，
    // 登出重登必须依靠解密缓存兜底（与 M6 产品承诺一致）
    bool clearUserData();
    // 关闭并删除本地数据库与存储密钥（彻底销毁，旧密文不可再恢复；
    // 仅用于不再需要重登解密的场景）
    void closeAndDestroy();
    bool isOpen() const { return m_open; }
    QString username() const { return m_username; }

    // 持久化 outbox（正文加密存储）；M7a: conversationId > 0 表示群消息
    bool addOutboxItem(const QString &clientMessageId, qint64 toUserId,
                       const QString &plaintext, qint64 conversationId = 0);
    bool removeOutboxItem(const QString &clientMessageId);
    QList<OutboxItem> loadOutbox() const;

    // 消息缓存（content 传入/返回均为明文，落库时加密）
    // msg 需含 messageId/conversationId/content 等 sync_messages 响应字段；
    // undecryptable=true 且已有可解密正文时保留旧明文不覆盖
    bool upsertMessage(const QJsonObject &msg);
    // 批量落库：整批共用一次事务提交。逐条自动提交会为每行付一次 fsync，
    // 一页（最多 100 条）的同步导入因此成为会话切换卡顿的主要来源。
    // 消息页泵按时间片调用本函数（每片一次提交），故提交粒度是"片"而非"页"：
    // 页越大 fsync 次数仍远少于逐条，且单片工作量有上界
    // 单行失败（字段非法/加密失败）只跳过该行：行级故障与整页无关，
    // 回滚整批会让好行也一起丢缓存
    bool upsertMessages(const QJsonArray &msgs);
    // 按 messageId 升序返回该会话最近 limit 条消息（字段同服务端响应）
    QJsonArray loadMessages(qint64 conversationId, int limit = 100) const;
    // 首页加载的逐行版本：只取原始行（正文密文放在 contentCipher，未解密），
    // 调用方可在时间片内逐行 decryptRowContent，避免整页解密阻塞 GUI 线程；
    // loadMessages 即"取行 + 逐行解密"的同步封装
    QJsonArray loadMessageRows(qint64 conversationId, int limit = 100) const;
    // 就地把行的 contentCipher 解密为 content：deleted 行置空、解不出置
    // undecryptable（与 loadMessages 同一实现，两条加载路径语义一致）
    void decryptRowContent(QJsonObject &row) const;
    // 读取单条消息的本地解密正文（content_enc 解密；无或 undecryptable 返回空）
    QString loadMessageContent(qint64 messageId) const;
    bool updateMessageStatus(qint64 messageId, const QString &status);
    // M9 特性栈：消息编辑（覆盖正文并标记编辑）与删除（软删除清空正文）。
    // editedAt 非空时写入服务端编辑时间（本端编辑用当前时间，同步回填用服务端时间）
    bool updateMessageContent(qint64 messageId, const QString &plaintext,
                              const QString &editedAt);
    // 仅推进 edited_at、不触碰正文：用于编辑新正文解不出时回退保留既有可读正文
    // （一次性预密钥已消费 / 群 ratchet 已推进的离线重放场景）
    bool markMessageEdited(qint64 messageId, const QString &editedAt);
    bool markMessageDeleted(qint64 messageId);
    // M9: 已读游标多端同步——清零该会话未读角标，并把 readMessageId 及之前的
    // 对方消息（sender_id != selfUserId）标记为已读（status_rank 只前进）
    bool markConversationRead(qint64 conversationId, qint64 readMessageId, qint64 selfUserId);

    // 会话缓存（lastMessage 预览加密存储）
    bool upsertConversation(const QJsonObject &conv);
    // 批量落库（同 upsertMessages：整批一次提交）
    bool upsertConversations(const QJsonArray &convs);
    QJsonArray loadConversations() const;
    // 仅更新已存在的会话行（避免事件流缺字段时产生幻影会话）
    bool bumpConversationPreview(qint64 conversationId, const QString &preview,
                                 bool incrementUnread);
    // M9 特性栈：更新会话偏好（置顶/免打扰），仅更新已存在会话行（服务端权威）
    bool setConversationPrefs(qint64 conversationId, bool pinned, bool muted);
    // M10: 会话整表删除——清除该会话的全部本地缓存（消息、解密缓存、
    // 会话行、群 Sender Key/跳过密钥、在途 outbox），与服务端硬删除对齐
    bool deleteConversation(qint64 conversationId);

    // M11A A3: 桌面通知辅助查询
    // 检查会话是否被设为免打扰（muted）
    bool isConversationMuted(qint64 conversationId) const;
    // 获取会话显示名称（私聊返回 peerUsername，群聊返回 name）
    QString conversationDisplayName(qint64 conversationId) const;
    // 获取会话类型（"private" 或 "group"），不存在返回空
    QString conversationType(qint64 conversationId) const;

    // M11A A5: 本地消息搜索
    // 在本地缓存中搜索消息正文（解密后 LIKE 匹配），返回匹配的消息数组。
    // 每条结果含 messageId/conversationId/senderUsername/content/createdAt。
    // 搜索范围：全部会话（conversationId <= 0）或指定会话。
    // 性能：千级消息量下可接受（< 1s），万级需考虑 FTS5 或异步搜索。
    QJsonArray searchMessages(const QString &query, int limit = 50,
                              qint64 conversationId = 0) const;

    // 解密缓存（messageId -> 明文，归口替代 M6 KeyStorage .cache）
    QString loadDecryptedContent(qint64 messageId) const;
    bool saveDecryptedContent(qint64 messageId, const QString &plaintext);
    // M9 特性栈：清除某消息的解密缓存（编辑后新密文解密前需先失效旧明文缓存）
    bool clearDecryptedContent(qint64 messageId);
    // 一次性导入并删除 M6 遗留的 KeyStorage 解密缓存文件，返回导入条数
    int importLegacyDecryptCache(const QString &username, const QString &deviceId);

    // sync_events 增量同步游标
    qint64 syncCursor() const;
    bool setSyncCursor(qint64 seq);

    // M7b: 群聊 Sender Key 本地持久化（chainKey 经存储密钥加密）
    bool saveSenderKey(qint64 groupId, qint64 senderUserId, const QString &senderDeviceId,
                       const QString &keyId, const QByteArray &chainKey,
                       const QByteArray &publicSigningKey,
                       const QByteArray &privateSigningKey, int iteration);
    bool loadSenderKey(qint64 groupId, qint64 senderUserId, const QString &senderDeviceId,
                       const QString &keyId, QByteArray &chainKey,
                       QByteArray &publicSigningKey,
                       QByteArray &privateSigningKey, int &iteration) const;
    // 按 groupId + senderUserId + senderDeviceId 返回最新的 keyId（若无返回空）。
    // “最新”以最近一次写入为准（rowid 降序）：saveSenderKey 用 INSERT OR REPLACE，
    // 每次写入都会获得更大的 rowid，因此结果确定。旧实现按 updated_at 排序并以
    // key_id 作并列破口，而 updated_at 为秒级精度、key_id 为随机 hex，导致同一秒内
    // 写入的两把密钥（轮换场景）选中哪一把完全随机，可能用陈旧密钥加密
    QString latestSenderKeyId(qint64 groupId, qint64 senderUserId,
                              const QString &senderDeviceId) const;
    bool removeSenderKeysForGroup(qint64 groupId);
    // M9 修复：历史 message_edited 事件/推送 payload 可能不带 senderId，而 Sender Key
    // 以（群, 发送者, 设备, keyId）定位；按设备 + keyId 反查发送者 userId（无则 0）
    qint64 senderUserIdForKey(qint64 groupId, const QString &senderDeviceId,
                              const QString &keyId) const;

    // M9 修复：群 Sender-Key 的“已跳过消息密钥”缓存持久化（密文落库）。
    // 用于容忍乱序投递与编辑重加密造成的 iteration 与消息 id 顺序解耦；
    // 属 E2EE 密钥材料，登出时与 sender_keys 一并保留
    bool saveSkippedMessageKeys(qint64 groupId, qint64 senderUserId,
                                const QString &senderDeviceId, const QString &keyId,
                                const QMap<int, QByteArray> &keys);
    QMap<int, QByteArray> loadSkippedMessageKeys(qint64 groupId, qint64 senderUserId,
                                                 const QString &senderDeviceId,
                                                 const QString &keyId) const;

    static QString dbFilePath(const QString &username, const QString &deviceId);

private:
    bool ensureSchema();
    // 行级 upsert：假定连接已就绪，供单条与批量两条路径共用
    bool upsertMessageRow(const QJsonObject &msg);
    bool upsertConversationRow(const QJsonObject &conv);
    // M7a: 列存在性检查（存量库幂等补列）
    bool hasColumn(const QString &table, const QString &column) const;
    bool ensureStorageKey(const QString &username, const QString &deviceId);
    // 建立 SQLite 连接（open 与失效自愈共用；前置：存储密钥已就绪）
    bool connectDatabase();
    // 审查修复：写路径入口统一校验连接；连接意外失效（陈旧句柄/驱动异常）
    // 时按原参数重开，重开失败则 fail-closed 禁用缓存，绝不带病执行 SQL
    bool ensureUsableDb();
    // 历史缺陷自愈：旧版本曾把 envelope 密文误存为正文，打开时检出并
    // 清空为 undecryptable（正文由后续重新同步 + 解密缓存恢复）
    void healEnvelopeLeaks();
    // AES-256-GCM 文本加解密；格式 "enc1:<base64(iv)>:<base64(密文+标签)>"，
    // 解密失败（含格式不符）返回空：宁缺毋滥，绝不回退明文
    QString encryptText(const QString &plaintext) const;
    QString decryptText(const QString &cipher) const;
    void closeDatabase();

    QSqlDatabase m_db;
    QString m_connectionName;
    QString m_username;
    QString m_deviceId;
    QByteArray m_storageKey; // 32 字节 AES-256-GCM 密钥，仅驻留进程内存
    bool m_open = false;
};
