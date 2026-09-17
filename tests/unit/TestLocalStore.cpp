/**
 * M6.5: LocalStore 本地加密持久化缓存单元测试
 *
 * 覆盖：schema/游标持久化、持久化 outbox、消息与会话缓存、
 *       磁盘密文不可读（fail-closed）、解密缓存归口与遗留迁移、
 *       登出销毁（closeAndDestroy）。
 */
#include <QtTest>
#include <QFile>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUuid>

#include "KeyStorage.h"
#include "LocalStore.h"
#include "encryption/E2eeCrypto.h"
#include "encryption/FileCrypto.h"
#include "encryption/GroupE2eeCrypto.h"
#include "protocol/FileProtocol.h"

namespace
{
const QString DeviceId = "testdevice";

QString uniqueUser()
{
    return "user_"
        + QUuid::createUuid().toString(QUuid::Id128).left(12).toLower();
}

QJsonObject makeMessage(qint64 messageId, qint64 conversationId, const QString &content,
                        qint64 senderId = 2, const QString &senderUsername = "bob",
                        const QString &status = "delivered")
{
    QJsonObject msg;
    msg["messageId"] = messageId;
    msg["conversationId"] = conversationId;
    msg["senderId"] = senderId;
    msg["senderUsername"] = senderUsername;
    msg["content"] = content;
    msg["contentType"] = "text";
    msg["status"] = status;
    msg["createdAt"] = "2026-08-21T00:00:00Z";
    return msg;
}

QByteArray readRawDb(const QString &user)
{
    QFile file(LocalStore::dbFilePath(user, DeviceId));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}
} // namespace

class TestLocalStore : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QCoreApplication::setOrganizationName("XYChat");
        QCoreApplication::setApplicationName("XYChatTestLocalStore");
        // 重定向 QStandardPaths 到测试目录，避免污染真实 AppData
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(!QStandardPaths::writableLocation(QStandardPaths::AppDataLocation).isEmpty());
    }

    // schema 与同步游标
    void schemaAndSyncCursorPersistAcrossReopen()
    {
        const QString user = uniqueUser();
        LocalStore store;
        QVERIFY(store.open(user, DeviceId));
        QCOMPARE(store.syncCursor(), 0);
        QVERIFY(store.setSyncCursor(42));
        store.close();

        // 模拟应用重启：重新打开后游标仍在
        QVERIFY(store.open(user, DeviceId));
        QCOMPARE(store.syncCursor(), 42);
        store.closeAndDestroy();
    }

    // 持久化 outbox
    void outboxPersistsAcrossReopen()
    {
        const QString user = uniqueUser();
        LocalStore store;
        QVERIFY(store.open(user, DeviceId));
        QVERIFY(store.addOutboxItem("cmid-1", 7, "outbox plain 1"));
        QVERIFY(store.addOutboxItem("cmid-2", 8, "outbox plain 2"));
        store.close();

        // 重启后 outbox 完整恢复（含解密后的明文）
        QVERIFY(store.open(user, DeviceId));
        const auto items = store.loadOutbox();
        QCOMPARE(items.size(), 2);
        QCOMPARE(items.at(0).clientMessageId, "cmid-1");
        QCOMPARE(items.at(0).toUserId, 7LL);
        QCOMPARE(items.at(0).content, "outbox plain 1");

        // 确认后删除（幂等键）
        QVERIFY(store.removeOutboxItem("cmid-1"));
        QCOMPARE(store.loadOutbox().size(), 1);
        store.closeAndDestroy();
    }

    void outboxContentEncryptedOnDisk()
    {
        const QString user = uniqueUser();
        const QString secret = "OutboxTopSecret-M6.5-Probe";
        LocalStore store;
        QVERIFY(store.open(user, DeviceId));
        QVERIFY(store.addOutboxItem("cmid-x", 9, secret));
        store.close();

        // fail-closed 验收：磁盘文件中不存在可读明文
        const QByteArray raw = readRawDb(user);
        QVERIFY(!raw.isEmpty());
        QVERIFY(!raw.contains(secret.toUtf8()));

        QVERIFY(store.open(user, DeviceId));
        QCOMPARE(store.loadOutbox().size(), 1);
        store.closeAndDestroy();
    }

    // M7a: 群 outbox（conversationId 目标）与群会话缓存字段
    void groupOutboxAndConversationFields()
    {
        const QString user = uniqueUser();
        LocalStore store;
        QVERIFY(store.open(user, DeviceId));
        // 群消息 outbox：toUserId=0、conversationId 为目标，跨重启保留
        QVERIFY(store.addOutboxItem("gcmid-1", 0, "group msg", 21));
        // 非法目标（两者均无效）被拒绝
        QVERIFY(!store.addOutboxItem("gcmid-bad", 0, "no target", 0));
        store.close();

        QVERIFY(store.open(user, DeviceId));
        const auto items = store.loadOutbox();
        QCOMPARE(items.size(), 1);
        QCOMPARE(items.at(0).clientMessageId, "gcmid-1");
        QCOMPARE(items.at(0).conversationId, 21LL);
        QCOMPARE(items.at(0).toUserId, 0LL);
        QCOMPARE(items.at(0).content, "group msg");

        // 群会话缓存：群名与成员数落库并可读回
        QJsonObject conv;
        conv["conversationId"] = 21;
        conv["type"] = "group";
        conv["name"] = "项目群";
        conv["memberCount"] = 3;
        conv["lastMessage"] = "hello";
        conv["lastMessageId"] = 5;
        conv["lastMessageAt"] = "2026-08-21T00:00:00Z";
        conv["unreadCount"] = 1;
        QVERIFY(store.upsertConversation(conv));

        const QJsonArray convs = store.loadConversations();
        QCOMPARE(convs.size(), 1);
        const QJsonObject loaded = convs.at(0).toObject();
        QCOMPARE(loaded.value("type").toString(), QString("group"));
        QCOMPARE(loaded.value("name").toString(), QString("项目群"));
        QCOMPARE(loaded.value("memberCount").toInt(), 3);
        QCOMPARE(loaded.value("lastMessage").toString(), QString("hello"));

        // 私聊会话不受群字段影响（默认空/0）
        QJsonObject privateConv;
        privateConv["conversationId"] = 22;
        privateConv["type"] = "private";
        privateConv["peerUserId"] = 7;
        privateConv["peerUsername"] = "bob";
        QVERIFY(store.upsertConversation(privateConv));
        const QJsonArray convs2 = store.loadConversations();
        QCOMPARE(convs2.size(), 2);
        bool foundPrivate = false;
        for (const QJsonValue &v : convs2) {
            const QJsonObject c = v.toObject();
            if (c.value("conversationId").toVariant().toLongLong() == 22) {
                foundPrivate = true;
                QCOMPARE(c.value("name").toString(), QString());
                QCOMPARE(c.value("memberCount").toInt(), 0);
            }
        }
        QVERIFY(foundPrivate);

        store.closeAndDestroy();
    }

    // M7b: 群聊 Sender Key 持久化（chainKey、签名公私钥、iteration）
    void senderKeyPersistsAcrossReopen()
    {
        const QString user = uniqueUser();
        LocalStore store;
        QVERIFY(store.open(user, DeviceId));

        const auto key = XYChat::Security::GroupE2eeCrypto::generateSenderKey();
        QVERIFY(key.valid);
        QVERIFY(store.saveSenderKey(100, 7, "deviceA", key.keyId,
                                    key.chainKey, key.publicSigningKey,
                                    key.privateSigningKey, key.iteration));

        const QString latest = store.latestSenderKeyId(100, 7, "deviceA");
        QCOMPARE(latest, key.keyId);

        QByteArray chainKey;
        QByteArray publicKey;
        QByteArray privateKey;
        int iteration = -1;
        QVERIFY(store.loadSenderKey(100, 7, "deviceA", key.keyId,
                                  chainKey, publicKey, privateKey, iteration));
        QCOMPARE(chainKey, key.chainKey);
        QCOMPARE(publicKey, key.publicSigningKey);
        QCOMPARE(privateKey, key.privateSigningKey);
        QCOMPARE(iteration, key.iteration);
        store.close();

        // 重启后仍能读取（chainKey 与私钥经存储密钥加密落库）
        QVERIFY(store.open(user, DeviceId));
        QVERIFY(store.loadSenderKey(100, 7, "deviceA", key.keyId,
                                  chainKey, publicKey, privateKey, iteration));
        QCOMPARE(iteration, key.iteration);
        store.closeAndDestroy();
    }

    void senderKeyLatestSelectsMostRecent()
    {
        const QString user = uniqueUser();
        LocalStore store;
        QVERIFY(store.open(user, DeviceId));

        const auto key1 = XYChat::Security::GroupE2eeCrypto::generateSenderKey();
        const auto key2 = XYChat::Security::GroupE2eeCrypto::generateSenderKey();
        QVERIFY(store.saveSenderKey(200, 8, "deviceB", key1.keyId,
                                    key1.chainKey, key1.publicSigningKey,
                                    key1.privateSigningKey, 1));
        QVERIFY(store.saveSenderKey(200, 8, "deviceB", key2.keyId,
                                    key2.chainKey, key2.publicSigningKey,
                                    key2.privateSigningKey, 2));

        QCOMPARE(store.latestSenderKeyId(200, 8, "deviceB"), key2.keyId);

        // “最新”= 最近一次写入：重新触碰 key1 后应改选 key1。旧实现按 updated_at
        // 秒级排序并以随机 hex 的 key_id 作并列破口，同一秒内写入两把密钥（轮换
        // 场景）时结果不确定，本用例约 50% 失败（曾被误归因为沙箱 DPAPI 偶发）
        QVERIFY(store.saveSenderKey(200, 8, "deviceB", key1.keyId,
                                    key1.chainKey, key1.publicSigningKey,
                                    key1.privateSigningKey, 3));
        QCOMPARE(store.latestSenderKeyId(200, 8, "deviceB"), key1.keyId);
        store.closeAndDestroy();
    }

    // M9 修复：历史 message_edited 事件/推送不带 senderId 时，按设备 + keyId 反查
    // 发送者（群聊解密靠 senderUserId 定位 Sender Key，缺失即永久不可解）
    void senderUserIdForKeyResolvesWithoutSenderId()
    {
        const QString user = uniqueUser();
        LocalStore store;
        QVERIFY(store.open(user, DeviceId));

        const auto key = XYChat::Security::GroupE2eeCrypto::generateSenderKey();
        QVERIFY(store.saveSenderKey(500, 21, "deviceF", key.keyId, key.chainKey,
                                    key.publicSigningKey, QByteArray(), 4));
        QCOMPARE(store.senderUserIdForKey(500, "deviceF", key.keyId), qint64(21));
        // 未知维度不得误匹配
        QCOMPARE(store.senderUserIdForKey(500, "deviceF", "nosuchkey"), qint64(0));
        QCOMPARE(store.senderUserIdForKey(501, "deviceF", key.keyId), qint64(0));
        QCOMPARE(store.senderUserIdForKey(500, "deviceG", key.keyId), qint64(0));
        store.closeAndDestroy();
    }

    // M9 修复：跳序消息密钥缓存的加密持久化（乱序投递与群消息编辑重加密依赖），
    // 属 E2EE 密钥材料：磁盘上必须是密文，且退群时随 sender_keys 一并清理
    void skippedMessageKeysPersistAcrossReopen()
    {
        const QString user = uniqueUser();
        LocalStore store;
        QVERIFY(store.open(user, DeviceId));

        QMap<int, QByteArray> keys;
        keys.insert(3, QByteArray(32, '\x11'));
        keys.insert(7, QByteArray(32, '\x22'));
        QVERIFY(store.saveSkippedMessageKeys(400, 11, "deviceD", "keyD", keys));
        store.close();

        // 磁盘上不得出现密钥明文的 base64（与消息正文同一加密落库约束）
        const QByteArray raw = readRawDb(user);
        QVERIFY(!raw.isEmpty());
        QVERIFY(!raw.contains(keys.value(3).toBase64()));
        QVERIFY(!raw.contains(keys.value(7).toBase64()));

        QVERIFY(store.open(user, DeviceId));
        const QMap<int, QByteArray> loaded =
            store.loadSkippedMessageKeys(400, 11, "deviceD", "keyD");
        QCOMPARE(loaded.size(), 2);
        QCOMPARE(loaded.value(3), keys.value(3));
        QCOMPARE(loaded.value(7), keys.value(7));

        // 维度隔离：群/发送者/设备/keyId 任一不同均不得命中
        QVERIFY(store.loadSkippedMessageKeys(401, 11, "deviceD", "keyD").isEmpty());
        QVERIFY(store.loadSkippedMessageKeys(400, 12, "deviceD", "keyD").isEmpty());
        QVERIFY(store.loadSkippedMessageKeys(400, 11, "deviceE", "keyD").isEmpty());
        QVERIFY(store.loadSkippedMessageKeys(400, 11, "deviceD", "keyX").isEmpty());

        // 空缓存写回即删行（避免陈旧密钥材料残留）
        QVERIFY(store.saveSkippedMessageKeys(400, 11, "deviceD", "keyD",
                                             QMap<int, QByteArray>()));
        QVERIFY(store.loadSkippedMessageKeys(400, 11, "deviceD", "keyD").isEmpty());

        // 退群清理连同跳序缓存一并删除
        QVERIFY(store.saveSkippedMessageKeys(400, 11, "deviceD", "keyD", keys));
        QVERIFY(store.removeSenderKeysForGroup(400));
        QVERIFY(store.loadSkippedMessageKeys(400, 11, "deviceD", "keyD").isEmpty());
        store.closeAndDestroy();
    }

    void senderKeyRemoveForGroupIsolated()
    {
        const QString user = uniqueUser();
        LocalStore store;
        QVERIFY(store.open(user, DeviceId));

        const auto key = XYChat::Security::GroupE2eeCrypto::generateSenderKey();
        QVERIFY(store.saveSenderKey(300, 9, "deviceC", key.keyId,
                                    key.chainKey, key.publicSigningKey,
                                    key.privateSigningKey, 0));
        QVERIFY(store.removeSenderKeysForGroup(300));
        QVERIFY(store.latestSenderKeyId(300, 9, "deviceC").isEmpty());
        store.closeAndDestroy();
    }

    // 消息缓存
    void messageContentEncryptedOnDisk()
    {
        const QString user = uniqueUser();
        const QString secret = "MessageTopSecret-M6.5-Probe";
        LocalStore store;
        QVERIFY(store.open(user, DeviceId));
        QVERIFY(store.upsertMessage(makeMessage(1, 10, secret)));
        store.close();

        const QByteArray raw = readRawDb(user);
        QVERIFY(!raw.isEmpty());
        QVERIFY(!raw.contains(secret.toUtf8()));

        // 重启后仍可读出解密正文
        QVERIFY(store.open(user, DeviceId));
        const QJsonArray messages = store.loadMessages(10);
        QCOMPARE(messages.size(), 1);
        QCOMPARE(messages.at(0).toObject().value("content").toString(), secret);
        store.closeAndDestroy();
    }

    void loadMessagesOrderedAndStatusUpdates()
    {
        const QString user = uniqueUser();
        LocalStore store;
        QVERIFY(store.open(user, DeviceId));
        // 乱序写入，读取需按 messageId 升序
        QVERIFY(store.upsertMessage(makeMessage(3, 11, "third")));
        QVERIFY(store.upsertMessage(makeMessage(1, 11, "first")));
        QVERIFY(store.upsertMessage(makeMessage(2, 11, "second")));
        QVERIFY(store.upsertMessage(makeMessage(99, 12, "other conv")));

        const QJsonArray messages = store.loadMessages(11);
        QCOMPARE(messages.size(), 3);
        QCOMPARE(messages.at(0).toObject().value("messageId").toVariant().toLongLong(), 1LL);
        QCOMPARE(messages.at(1).toObject().value("messageId").toVariant().toLongLong(), 2LL);
        QCOMPARE(messages.at(2).toObject().value("messageId").toVariant().toLongLong(), 3LL);

        // 状态更新（已送达/已读推送落缓存）
        QVERIFY(store.updateMessageStatus(1, "read"));
        const QJsonArray updated = store.loadMessages(11);
        QCOMPARE(updated.at(0).toObject().value("status").toString(), "read");
        store.closeAndDestroy();
    }

    void upsertKeepsDecryptedOverUndecryptable()
    {
        const QString user = uniqueUser();
        LocalStore store;
        QVERIFY(store.open(user, DeviceId));
        QVERIFY(store.upsertMessage(makeMessage(5, 20, "decrypted plain")));

        // 重新同步到同一消息但暂不可解密：不得覆盖既有明文
        QJsonObject undecryptable = makeMessage(5, 20, QString());
        undecryptable["undecryptable"] = true;
        QVERIFY(store.upsertMessage(undecryptable));
        QJsonArray messages = store.loadMessages(20);
        QCOMPARE(messages.size(), 1);
        QCOMPARE(messages.at(0).toObject().value("content").toString(),
                 "decrypted plain");
        QVERIFY(!messages.at(0).toObject().contains("undecryptable"));

        // 新的可解密正文允许覆盖
        QVERIFY(store.upsertMessage(makeMessage(5, 20, "re-decrypted")));
        messages = store.loadMessages(20);
        QCOMPARE(messages.at(0).toObject().value("content").toString(),
                 "re-decrypted");
        store.closeAndDestroy();
    }

    // M12: 批量落库与逐行解密（会话切换的异步分片加载依赖这两条路径）
    void batchUpsertAndRowDecryptMatchSinglePath()
    {
        const QString user = uniqueUser();
        LocalStore store;
        QVERIFY(store.open(user, DeviceId));

        QJsonArray batch;
        for (int i = 1; i <= 5; ++i) {
            batch.append(makeMessage(i, 40, QString("batch %1").arg(i)));
        }
        QVERIFY(store.upsertMessages(batch));

        // 批量写入与单条写入等价：升序且在库中可解密
        const QJsonArray messages = store.loadMessages(40);
        QCOMPARE(messages.size(), 5);
        for (int i = 0; i < messages.size(); ++i) {
            const QJsonObject msg = messages.at(i).toObject();
            QCOMPARE(msg.value("messageId").toVariant().toLongLong(), qint64(i + 1));
            QCOMPARE(msg.value("content").toString(), QString("batch %1").arg(i + 1));
        }

        // 逐行路径（先取行、时间片内解密）与同步封装输出一致
        const QJsonArray rows = store.loadMessageRows(40);
        QCOMPARE(rows.size(), 5);
        for (int i = 0; i < rows.size(); ++i) {
            QJsonObject row = rows.at(i).toObject();
            QVERIFY2(row.contains("contentCipher"), "取行阶段不得携带明文");
            QVERIFY(!row.contains("content"));
            store.decryptRowContent(row);
            QVERIFY(!row.contains("contentCipher"));
            QCOMPARE(row.value("messageId").toVariant().toLongLong(), qint64(i + 1));
            QCOMPARE(row.value("content").toString(), QString("batch %1").arg(i + 1));
            QVERIFY(!row.contains("undecryptable"));
            // 与同步路径逐字段等价
            const QJsonObject reference = messages.at(i).toObject();
            QCOMPARE(row.value("contentType").toString(),
                     reference.value("contentType").toString());
            QCOMPARE(row.value("status").toString(), reference.value("status").toString());
            QCOMPARE(row.value("createdAt").toString(), reference.value("createdAt").toString());
            QCOMPARE(row.contains("deleted"), reference.contains("deleted"));
            QCOMPARE(row.contains("edited"), reference.contains("edited"));
        }

        // 软删除行：两条路径都不带出正文
        QVERIFY(store.upsertMessage(makeMessage(9, 40, "will be deleted")));
        QVERIFY(store.markMessageDeleted(9));
        const QJsonArray withDeleted = store.loadMessages(40);
        const QJsonObject deletedMsg = withDeleted.at(withDeleted.size() - 1).toObject();
        QCOMPARE(deletedMsg.value("messageId").toVariant().toLongLong(), qint64(9));
        QVERIFY(deletedMsg.value("deleted").toBool());
        QVERIFY(deletedMsg.value("content").toString().isEmpty());
        QJsonObject deletedRow = store.loadMessageRows(40).at(5).toObject();
        store.decryptRowContent(deletedRow);
        QVERIFY(deletedRow.value("deleted").toBool());
        QVERIFY(deletedRow.value("content").toString().isEmpty());

        // 批量会话写入同样落库
        QJsonArray convs;
        QJsonObject convA;
        convA["conversationId"] = 40;
        convA["type"] = "private";
        convA["peerUserId"] = 2;
        convA["peerUsername"] = "bob";
        convA["lastMessage"] = "batch 5";
        convA["lastMessageAt"] = "2026-08-21T00:00:00Z";
        QJsonObject convB = convA;
        convB["conversationId"] = 41;
        convB["lastMessage"] = "second";
        convs.append(convA);
        convs.append(convB);
        QVERIFY(store.upsertConversations(convs));
        QCOMPARE(store.loadConversations().size(), 2);
        store.closeAndDestroy();
    }

    // M12 补漏：本地搜索命中的正文同样不得把文件清单（含 32 字节密钥）
    // 交给 QML——降级为 "[File] 名"，绝不回退成清单原文
    void searchMessagesNeverReturnsFileManifest()
    {
        const QString user = uniqueUser();
        LocalStore store;
        QVERIFY(store.open(user, DeviceId));

        const auto fileKey = XYChat::Security::FileCrypto::generateFileKey();
        XYChat::Protocol::FileManifest manifest;
        manifest.fileId = 77;
        manifest.name = "searchable-report.pdf";
        manifest.mime = "application/pdf";
        manifest.chunkSize = 1024 * 1024;
        manifest.plainSize = XYChat::Protocol::plainSizeOfChunk(manifest.chunkSize);
        manifest.cipherSize = manifest.plainSize + 16;
        manifest.sha256Hex = XYChat::Security::FileCrypto::sha256Hex("ciphertext-placeholder");
        manifest.key = fileKey.key;
        manifest.iv = fileKey.iv;
        const QString manifestJson = XYChat::Protocol::encodeFileManifest(manifest);
        QVERIFY(XYChat::Protocol::looksLikeFileManifest(manifestJson));
        QVERIFY(store.upsertMessage(makeMessage(1, 45, manifestJson)));
        QVERIFY(store.upsertMessage(makeMessage(2, 45, "plain searchable text")));

        const QJsonArray hits = store.searchMessages("searchable");
        QCOMPARE(hits.size(), 2);
        bool sawPreview = false;
        for (const QJsonValue &value : hits) {
            const QJsonObject msg = value.toObject();
            const qint64 id = msg.value("messageId").toVariant().toLongLong();
            const QString content = msg.value("content").toString();
            if (id == 1) {
                QCOMPARE(content, QString("[File] searchable-report.pdf"));
                QVERIFY2(!content.contains(manifest.sha256Hex), "manifest leaked into search hits");
                sawPreview = true;
            } else {
                QCOMPARE(content, QString("plain searchable text"));
            }
        }
        QVERIFY(sawPreview);
        store.closeAndDestroy();
    }

    // 登出清理语义（运行期缺陷回归）
    void logoutClearsUserDataButKeepsDecryptCache()
    {
        const QString user = uniqueUser();
        LocalStore store;
        QVERIFY(store.open(user, DeviceId));
        QVERIFY(store.upsertMessage(makeMessage(1, 50, "visible message")));
        QVERIFY(store.addOutboxItem("cmid-l", 7, "pending"));
        QVERIFY(store.setSyncCursor(15));
        QVERIFY(store.saveDecryptedContent(1, "visible message"));

        // 登出语义：用户可见数据全部清除
        QVERIFY(store.clearUserData());
        QCOMPARE(store.loadMessages(50).size(), 0);
        QCOMPARE(store.loadOutbox().size(), 0);
        QCOMPARE(store.syncCursor(), 0);
        QCOMPARE(store.loadConversations().size(), 0);

        // 但解密缓存（E2EE 密钥材料）必须保留：预密钥已消费不可恢复，
        // 登出重登后对方消息只能靠它兜底（此前整库销毁导致重登无法解密）
        QCOMPARE(store.loadDecryptedContent(1), "visible message");
        store.close();

        // 重新登录（重开库）后仍在
        QVERIFY(store.open(user, DeviceId));
        QCOMPARE(store.loadDecryptedContent(1), "visible message");
        store.closeAndDestroy();
    }

    void upsertNeverPersistsEnvelopeCiphertext()
    {
        const QString user = uniqueUser();
        const QString envelope = "{\"v\":1,\"devices\":[]}";
        LocalStore store;
        QVERIFY(store.open(user, DeviceId));

        // 解密失败路径：undecryptable 标志 + content 残留 envelope 原文
        QJsonObject failed = makeMessage(1, 60, envelope);
        failed["undecryptable"] = true;
        QVERIFY(store.upsertMessage(failed));
        // 防御分支：无标志但 content 本身就是 envelope
        QVERIFY(store.upsertMessage(makeMessage(2, 60, envelope)));

        const QJsonArray messages = store.loadMessages(60);
        QCOMPARE(messages.size(), 2);
        for (const QJsonValue &value : messages) {
            const QJsonObject msg = value.toObject();
            QVERIFY2(msg.value("content").toString().isEmpty(),
                     "envelope ciphertext must never be cached as content");
            QVERIFY(msg.value("undecryptable").toBool());
        }
        store.closeAndDestroy();
    }

    void healsLegacyEnvelopeLeakRows()
    {
        const QString user = uniqueUser();
        LocalStore store;
        QVERIFY(store.open(user, DeviceId));
        QVERIFY(store.upsertMessage(makeMessage(1, 70, "healthy row")));
        store.close();

        // 模拟旧缺陷版本写入的污染行：envelope 原文以存储密钥加密落库
        const QByteArray key = KeyStorage::loadLocalStoreKey(user, DeviceId);
        QCOMPARE(key.size(), 32);
        const QString envelope = "{\"v\":1,\"devices\":[]}";
        const auto gcm = XYChat::Security::E2eeCrypto::aesGcmEncrypt(key, envelope.toUtf8());
        QVERIFY(gcm.valid);
        const QString enc = "enc1:"
            + QString::fromLatin1(gcm.iv.toBase64()) + QLatin1Char(':')
            + QString::fromLatin1(gcm.ciphertext.toBase64());
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE",
                                                        "healprobe");
            db.setDatabaseName(LocalStore::dbFilePath(user, DeviceId));
            QVERIFY(db.open());
            QSqlQuery query(db);
            query.prepare(
                "UPDATE messages SET content_enc = ?, undecryptable = 0 WHERE message_id = 1");
            query.addBindValue(enc);
            QVERIFY(query.exec());
            db.close();
        }
        QSqlDatabase::removeDatabase("healprobe");

        // 再次打开触发自愈：污染行被清空为 undecryptable，不再泄漏到 UI
        QVERIFY(store.open(user, DeviceId));
        const QJsonArray messages = store.loadMessages(70);
        QCOMPARE(messages.size(), 1);
        QVERIFY(messages.at(0).toObject().value("content").toString().isEmpty());
        QVERIFY(messages.at(0).toObject().value("undecryptable").toBool());
        store.closeAndDestroy();
    }

    // 群 envelope 密文拦截：group_e2ee / sender_key_distribution 原文绝不落库
    void upsertNeverPersistsGroupEnvelopeCiphertext()
    {
        const QString user = uniqueUser();
        const QString groupEnv =
            "{\"v\":1,\"type\":\"group_e2ee\",\"keyId\":\"k1\",\"iteration\":1,"
            "\"senderDeviceId\":\"devA\",\"iv\":\"aaa\",\"ct\":\"bbb\",\"sig\":\"ccc\"}";
        const QString distEnv =
            "{\"v\":1,\"type\":\"sender_key_distribution\",\"groupId\":10,"
            "\"senderUserId\":7,\"senderDeviceId\":\"devA\",\"keyId\":\"k1\",\"entries\":[]}";
        // 自校验：测试数据确实被识别为群 envelope（否则拦截断言无意义）
        QVERIFY(XYChat::Security::GroupE2eeCrypto::looksLikeGroupMessage(groupEnv));
        QVERIFY(XYChat::Security::GroupE2eeCrypto::looksLikeDistribution(distEnv));

        LocalStore store;
        QVERIFY(store.open(user, DeviceId));
        // 无 undecryptable 标志、content 直接是群 envelope 原文：也必须被拦截清空
        QVERIFY(store.upsertMessage(makeMessage(1, 80, groupEnv)));
        QVERIFY(store.upsertMessage(makeMessage(2, 80, distEnv)));

        const QJsonArray messages = store.loadMessages(80);
        QCOMPARE(messages.size(), 2);
        for (const QJsonValue &value : messages) {
            const QJsonObject msg = value.toObject();
            QVERIFY2(msg.value("content").toString().isEmpty(),
                     "group envelope ciphertext must never be cached as content");
            QVERIFY(msg.value("undecryptable").toBool());
        }
        store.closeAndDestroy();
    }

    void healsLegacyGroupEnvelopeLeakRows()
    {
        const QString user = uniqueUser();
        LocalStore store;
        QVERIFY(store.open(user, DeviceId));
        QVERIFY(store.upsertMessage(makeMessage(1, 90, "healthy row")));
        store.close();

        // 模拟旧缺陷版本写入的群密文污染行：group_e2ee envelope 原文加密落库
        const QByteArray key = KeyStorage::loadLocalStoreKey(user, DeviceId);
        QCOMPARE(key.size(), 32);
        const QString groupEnv =
            "{\"v\":1,\"type\":\"group_e2ee\",\"keyId\":\"k1\",\"iteration\":1,"
            "\"senderDeviceId\":\"devA\",\"iv\":\"aaa\",\"ct\":\"bbb\",\"sig\":\"ccc\"}";
        const auto gcm = XYChat::Security::E2eeCrypto::aesGcmEncrypt(key, groupEnv.toUtf8());
        QVERIFY(gcm.valid);
        const QString enc = "enc1:"
            + QString::fromLatin1(gcm.iv.toBase64()) + QLatin1Char(':')
            + QString::fromLatin1(gcm.ciphertext.toBase64());
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE",
                                                        "healgroupprobe");
            db.setDatabaseName(LocalStore::dbFilePath(user, DeviceId));
            QVERIFY(db.open());
            QSqlQuery query(db);
            query.prepare(
                "UPDATE messages SET content_enc = ?, undecryptable = 0 WHERE message_id = 1");
            query.addBindValue(enc);
            QVERIFY(query.exec());
            db.close();
        }
        QSqlDatabase::removeDatabase("healgroupprobe");

        // 再次打开触发自愈：群密文污染行被清空为 undecryptable，不再泄漏到 UI
        QVERIFY(store.open(user, DeviceId));
        const QJsonArray messages = store.loadMessages(90);
        QCOMPARE(messages.size(), 1);
        QVERIFY(messages.at(0).toObject().value("content").toString().isEmpty());
        QVERIFY(messages.at(0).toObject().value("undecryptable").toBool());
        store.closeAndDestroy();
    }

    // M9: 已读游标应用——未读角标按剩余未读重算（非无条件清零）、状态只前进、排除自己消息
    void markConversationReadRecomputesUnreadAndOnlyAdvances()
    {
        const QString user = uniqueUser();
        LocalStore store;
        QVERIFY(store.open(user, DeviceId));

        const qint64 selfId = 1;
        const qint64 convId = 5;

        QJsonObject conv;
        conv["conversationId"] = convId;
        conv["peerUserId"] = 2;
        conv["peerUsername"] = "bob";
        conv["lastMessage"] = "m2";
        conv["lastMessageAt"] = "2026-08-21T00:00:00Z";
        conv["unreadCount"] = 2;
        QVERIFY(store.upsertConversation(conv));

        // 对方消息 M1(10)、M2(20)；自己消息 M3(15)
        QVERIFY(store.upsertMessage(makeMessage(10, convId, "m1", 2, "bob", "delivered")));
        QVERIFY(store.upsertMessage(makeMessage(15, convId, "mine", selfId, "alice", "sent")));
        QVERIFY(store.upsertMessage(makeMessage(20, convId, "m2", 2, "bob", "delivered")));

        // 推进到 10：M1 已读，M2(>10) 仍未读，自己的 M3 不受影响
        QVERIFY(store.markConversationRead(convId, 10, selfId));
        QCOMPARE(store.loadConversations().at(0).toObject().value("unreadCount").toInt(), 1);

        const QJsonArray msgs = store.loadMessages(convId);
        QCOMPARE(msgs.size(), 3);
        for (const QJsonValue &v : msgs) {
            const QJsonObject m = v.toObject();
            const qint64 id = m.value("messageId").toVariant().toLongLong();
            const QString st = m.value("status").toString();
            if (id == 10) {
                QCOMPARE(st, QString("read"));
            } else if (id == 20) {
                QCOMPARE(st, QString("delivered"));
            } else if (id == 15) {
                QCOMPARE(st, QString("sent"));
            }
        }

        // 继续推进到 20：M2 也已读，未读清零
        QVERIFY(store.markConversationRead(convId, 20, selfId));
        QCOMPARE(store.loadConversations().at(0).toObject().value("unreadCount").toInt(), 0);

        store.closeAndDestroy();
    }

    // 回执状态只前进不回退
    void statusOnlyMovesForward()
    {
        const QString user = uniqueUser();
        LocalStore store;
        QVERIFY(store.open(user, DeviceId));
        QVERIFY(store.upsertMessage(makeMessage(1, 30, "m", 2, "bob", "delivered")));
        // 回执推送推进到已读
        QVERIFY(store.updateMessageStatus(1, "read"));

        // 滞后同步不得把 read 回退为 delivered（审查修复回归项）
        QVERIFY(store.upsertMessage(makeMessage(1, 30, "m", 2, "bob", "delivered")));
        QJsonArray messages = store.loadMessages(30);
        QCOMPARE(messages.at(0).toObject().value("status").toString(),
                 "read");

        // 前进方向覆盖允许（内容同步更新）
        QVERIFY(store.upsertMessage(makeMessage(1, 30, "m2", 2, "bob", "read")));
        messages = store.loadMessages(30);
        QCOMPARE(messages.at(0).toObject().value("status").toString(),
                 "read");
        QCOMPARE(messages.at(0).toObject().value("content").toString(),
                 "m2");
        store.closeAndDestroy();
    }

    // 会话缓存
    void conversationRoundTripAndPreviewBump()
    {
        const QString user = uniqueUser();
        const QString secretPreview = "SecretPreview-M6.5-Probe";
        LocalStore store;
        QVERIFY(store.open(user, DeviceId));

        QJsonObject conv;
        conv["conversationId"] = 5;
        conv["type"] = "private";
        conv["peerUserId"] = 7;
        conv["peerUsername"] = "alice";
        conv["lastMessage"] = secretPreview;
        conv["lastMessageId"] = 3;
        conv["lastMessageAt"] = "2026-08-21T00:00:00Z";
        conv["unreadCount"] = 2;
        QVERIFY(store.upsertConversation(conv));
        store.close();

        // 预览同样加密落库
        const QByteArray raw = readRawDb(user);
        QVERIFY(!raw.isEmpty());
        QVERIFY(!raw.contains(secretPreview.toUtf8()));

        QVERIFY(store.open(user, DeviceId));
        QJsonArray conversations = store.loadConversations();
        QCOMPARE(conversations.size(), 1);
        const QJsonObject loaded = conversations.at(0).toObject();
        QCOMPARE(loaded.value("conversationId").toVariant().toLongLong(), 5LL);
        QCOMPARE(loaded.value("peerUsername").toString(), "alice");
        QCOMPARE(loaded.value("lastMessage").toString(), secretPreview);
        QCOMPARE(loaded.value("unreadCount").toInt(), 2);

        // 新消息到达：预览与未读数更新
        QVERIFY(store.bumpConversationPreview(5, "new preview", true));
        conversations = store.loadConversations();
        QCOMPARE(conversations.at(0).toObject().value("lastMessage").toString(),
                 "new preview");
        QCOMPARE(conversations.at(0).toObject().value("unreadCount").toInt(), 3);

        // 不存在的会话不产生幻影行（事件流缺会话元数据时的保护）
        QVERIFY(store.bumpConversationPreview(99, "x", true));
        QCOMPARE(store.loadConversations().size(), 1);
        store.closeAndDestroy();
    }

    // 解密缓存归口与遗留迁移
    void decryptCacheLegacyImport()
    {
        const QString user = uniqueUser();
        QHash<qint64, QString> legacy;
        legacy.insert(101, "legacy-plain-1");
        legacy.insert(102, "legacy-plain-2");
        QVERIFY(KeyStorage::saveDecryptCache(user, DeviceId, legacy));

        LocalStore store;
        QVERIFY(store.open(user, DeviceId));
        QCOMPARE(store.importLegacyDecryptCache(user, DeviceId), 2);
        QCOMPARE(store.loadDecryptedContent(101), "legacy-plain-1");
        QCOMPARE(store.loadDecryptedContent(102), "legacy-plain-2");

        // 遗留文件已删除（二次导入为空操作）
        QVERIFY(KeyStorage::loadDecryptCache(user, DeviceId).isEmpty());
        QCOMPARE(store.importLegacyDecryptCache(user, DeviceId), 0);

        // 新缓存写入/读取
        QVERIFY(store.saveDecryptedContent(103, "fresh-plain"));
        QCOMPARE(store.loadDecryptedContent(103), "fresh-plain");
        store.closeAndDestroy();
    }

    // 登出销毁
    void destroyRemovesLocalData()
    {
        const QString user = uniqueUser();
        LocalStore store;
        QVERIFY(store.open(user, DeviceId));
        QVERIFY(store.upsertMessage(makeMessage(1, 10, "to be destroyed")));
        QVERIFY(store.addOutboxItem("cmid-d", 7, "gone"));
        QVERIFY(store.setSyncCursor(7));
        store.closeAndDestroy();

        // 数据库文件与存储密钥均被删除，旧密文不可再恢复
        QVERIFY(!QFile::exists(LocalStore::dbFilePath(user, DeviceId)));
        QVERIFY(KeyStorage::loadLocalStoreKey(user, DeviceId).isEmpty());

        // 重新登录（再次 open）得到全新空库
        QVERIFY(store.open(user, DeviceId));
        QCOMPARE(store.syncCursor(), 0);
        QCOMPARE(store.loadMessages(10).size(), 0);
        QCOMPARE(store.loadOutbox().size(), 0);
        store.closeAndDestroy();
    }

    // M11A A5: 本地消息搜索
    void searchMessagesFindsMatchingContent()
    {
        const QString user = uniqueUser();
        LocalStore store;
        QVERIFY(store.open(user, DeviceId));

        // 写入测试消息
        QVERIFY(store.upsertMessage(makeMessage(1, 10, "Hello world")));
        QVERIFY(store.upsertMessage(makeMessage(2, 10, "Goodbye world")));
        QVERIFY(store.upsertMessage(makeMessage(3, 11, "Hello there")));
        QVERIFY(store.upsertMessage(makeMessage(4, 11, "Nothing special")));

        // 搜索 "hello"（大小写不敏感）
        QJsonArray results = store.searchMessages("hello", 50, 0);
        QCOMPARE(results.size(), 2);
        // 结果按 messageId 降序（最近优先）
        QCOMPARE(results.at(0).toObject().value("messageId").toVariant().toLongLong(), 3LL);
        QCOMPARE(results.at(1).toObject().value("messageId").toVariant().toLongLong(), 1LL);

        // 搜索 "world"
        results = store.searchMessages("world", 50, 0);
        QCOMPARE(results.size(), 2);

        // 搜索不存在的关键词
        results = store.searchMessages("nonexistent", 50, 0);
        QCOMPARE(results.size(), 0);

        // 限制搜索范围到特定会话
        results = store.searchMessages("hello", 50, 10);
        QCOMPARE(results.size(), 1);
        QCOMPARE(results.at(0).toObject().value("messageId").toVariant().toLongLong(), 1LL);

        // 限制结果数量
        results = store.searchMessages("o", 1, 0); // "o" 匹配所有含 o 的消息
        QCOMPARE(results.size(), 1);

        store.closeAndDestroy();
    }

    // M11A A3: 会话免打扰与显示名称查询
    void conversationMutedAndDisplayName()
    {
        const QString user = uniqueUser();
        LocalStore store;
        QVERIFY(store.open(user, DeviceId));

        // 私聊会话
        QJsonObject privateConv;
        privateConv["conversationId"] = 10;
        privateConv["type"] = "private";
        privateConv["peerUserId"] = 7;
        privateConv["peerUsername"] = "alice";
        privateConv["muted"] = false;
        QVERIFY(store.upsertConversation(privateConv));

        // 群聊会话（免打扰）
        QJsonObject groupConv;
        groupConv["conversationId"] = 20;
        groupConv["type"] = "group";
        groupConv["name"] = "项目群";
        groupConv["muted"] = true;
        QVERIFY(store.upsertConversation(groupConv));

        // 检查免打扰状态
        QVERIFY(!store.isConversationMuted(10));
        QVERIFY(store.isConversationMuted(20));
        QVERIFY(!store.isConversationMuted(999)); // 不存在的会话

        // 检查显示名称
        QCOMPARE(store.conversationDisplayName(10), QString("alice"));
        QCOMPARE(store.conversationDisplayName(20), QString("项目群"));
        QVERIFY(store.conversationDisplayName(999).isEmpty());

        store.closeAndDestroy();
    }
};

QTEST_GUILESS_MAIN(TestLocalStore)
#include "TestLocalStore.moc"
