#include <QtTest>
#include <QSignalSpy>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <QTimeZone>

#include "NetworkManager.h"
#include "FileCrypto.h"
#include "FileProtocol.h"

namespace FileProtocol = XYChat::Protocol;
using XYChat::Security::FileCrypto;

using XYChat::Protocol::Packet;
using XYChat::Protocol::MessageType;
using XYChat::Protocol::ErrorCode;

// M9 欠账修复：客户端链路层（NetworkManager）回归单测。
// 覆盖周度审查暴露的缺陷面——编辑/删除响应匹配（多槽不再静默丢弃）、
// 编辑/删除推送的发起设备去重、私聊编辑队列化、会话过期时间解析。
// 通过 friend 注入私有状态，避免真实 socket/密钥：LocalStore 不打开，
// store 分支被安全跳过，信号仍同步发射可被 QSignalSpy 捕获。
class TestNetworkManager : public QObject
{
    Q_OBJECT

private:
    static QByteArray payloadBytes(const QJsonObject &obj)
    {
        return QJsonDocument(obj).toJson(QJsonDocument::Compact);
    }

private slots:
    // 会话过期时间解析：空/非法返回 0；无时区 ISO 串经本地偏移校正后
    // 应与带 Z 的 UTC 串等价（这是 parseExpiresAt 的核心目的，时区无关）
    void parseExpiresAtHandlesEmptyInvalidAndTimezone()
    {
        NetworkManager nm;

        QCOMPARE(nm.parseExpiresAt(QString()), qint64(0));
        QCOMPARE(nm.parseExpiresAt("not-a-date"), qint64(0));

        const qint64 utc = nm.parseExpiresAt("2026-09-05T12:00:00Z");
        const qint64 naive = nm.parseExpiresAt("2026-09-05T12:00:00");
        QVERIFY(utc > 0);
        // 服务端以 UTC 生成无时区后缀的 ISO 串；校正后应与显式 UTC 一致
        QCOMPARE(naive, utc);

        // 基准时刻核对：2026-09-05T12:00:00Z
        const QDateTime ground(QDate(2026, 9, 5), QTime(12, 0, 0), QTimeZone::UTC);
        QCOMPARE(utc, ground.toSecsSinceEpoch());

        // 单调性：更晚的时间戳更大
        QVERIFY(nm.parseExpiresAt("2026-09-05T13:00:00Z") > utc);
    }

    // 编辑响应匹配：requestId 命中 m_pendingEdits 才处理并消费；不命中静默忽略
    void editResponseMatchesPendingEditOnly()
    {
        NetworkManager nm;
        nm.m_pendingEdits.insert(42, NetworkManager::EditContext{100, 5, "newtext"});

        QSignalSpy editedSpy(&nm, &NetworkManager::messageEdited);

        // 不匹配的 requestId：忽略，上下文保留
        Packet mismatch;
        mismatch.messageType = MessageType::EditMessageResponse;
        mismatch.requestId = 99;
        QJsonObject mdata;
        mdata["code"] = static_cast<int>(ErrorCode::Ok);
        mdata["data"] = QJsonObject{{"conversationId", 5}, {"editedAt", "t"}};
        mismatch.payload = payloadBytes(mdata);
        nm.handleEditMessageResponse(mismatch);
        QCOMPARE(editedSpy.count(), 0);
        QVERIFY(nm.m_pendingEdits.contains(42));

        // 匹配的 requestId：消费并发射 messageEdited（携乐观明文）
        Packet match;
        match.messageType = MessageType::EditMessageResponse;
        match.requestId = 42;
        QJsonObject data;
        data["code"] = static_cast<int>(ErrorCode::Ok);
        data["data"] = QJsonObject{{"conversationId", 5}, {"editedAt", "2026-09-05T12:00:00Z"}};
        match.payload = payloadBytes(data);
        nm.handleEditMessageResponse(match);
        QCOMPARE(editedSpy.count(), 1);
        QVERIFY(!nm.m_pendingEdits.contains(42));
        const QList<QVariant> args = editedSpy.takeFirst();
        QCOMPARE(args.at(0).toLongLong(), qint64(5));   // conversationId
        QCOMPARE(args.at(1).toLongLong(), qint64(100));  // messageId（取自上下文）
        QCOMPARE(args.at(2).toString(), "newtext");      // 编辑后明文

        // 重复同 requestId：已消费，不再发射
        nm.handleEditMessageResponse(match);
        QCOMPARE(editedSpy.count(), 0);
    }

    // 编辑响应错误码：非 Ok 上报 messageEditFailed 并移除上下文
    void editResponseFailureReportsAndConsumes()
    {
        NetworkManager nm;
        nm.m_pendingEdits.insert(7, NetworkManager::EditContext{50, 3, "x"});
        QSignalSpy failedSpy(&nm, &NetworkManager::messageEditFailed);

        Packet p;
        p.messageType = MessageType::EditMessageResponse;
        p.requestId = 7;
        QJsonObject json;
        json["code"] = static_cast<int>(ErrorCode::RateLimited);
        json["message"] = "Too many edits";
        p.payload = payloadBytes(json);
        nm.handleEditMessageResponse(p);

        QCOMPARE(failedSpy.count(), 1);
        QVERIFY(!nm.m_pendingEdits.contains(7));
    }

    // 删除响应匹配：requestId 命中集合才处理并移除
    void deleteResponseMatchesPendingSet()
    {
        NetworkManager nm;
        nm.m_pendingDeleteRequestIds.insert(11);
        QSignalSpy deletedSpy(&nm, &NetworkManager::messageDeleted);

        Packet mismatch;
        mismatch.messageType = MessageType::DeleteMessageResponse;
        mismatch.requestId = 12;
        QJsonObject mj;
        mj["code"] = static_cast<int>(ErrorCode::Ok);
        mj["data"] = QJsonObject{{"messageId", 200}, {"conversationId", 5}};
        mismatch.payload = payloadBytes(mj);
        nm.handleDeleteMessageResponse(mismatch);
        QCOMPARE(deletedSpy.count(), 0);
        QVERIFY(nm.m_pendingDeleteRequestIds.contains(11));

        Packet match;
        match.messageType = MessageType::DeleteMessageResponse;
        match.requestId = 11;
        QJsonObject j;
        j["code"] = static_cast<int>(ErrorCode::Ok);
        j["data"] = QJsonObject{{"messageId", 200}, {"conversationId", 5}};
        match.payload = payloadBytes(j);
        nm.handleDeleteMessageResponse(match);
        QCOMPARE(deletedSpy.count(), 1);
        QVERIFY(!nm.m_pendingDeleteRequestIds.contains(11));
    }

    // 删除推送：本端发起设备（senderId 与 originDeviceId 同时命中）去重，
    // 他人（不同 senderId）正常处理并发射 messageDeleted（无需密钥）
    void deleteNotificationDedupsOwnOrigin()
    {
        NetworkManager nm;
        nm.m_userId = 7;
        nm.m_localDeviceId = "devA";
        QSignalSpy deletedSpy(&nm, &NetworkManager::messageDeleted);

        Packet own;
        own.messageType = MessageType::MessageDeletedNotification;
        own.requestId = 0;
        own.payload = payloadBytes(QJsonObject{
            {"messageId", 200}, {"conversationId", 5},
            {"senderId", 7}, {"originDeviceId", "devA"}});
        nm.handleMessageDeletedNotification(own);
        QCOMPARE(deletedSpy.count(), 0);  // 本端其他设备发起，去重

        // 同机不同账号（deviceId 相同但 senderId 不同）不得被误去重
        Packet foreignSameDevice;
        foreignSameDevice.messageType = MessageType::MessageDeletedNotification;
        foreignSameDevice.requestId = 0;
        foreignSameDevice.payload = payloadBytes(QJsonObject{
            {"messageId", 201}, {"conversationId", 5},
            {"senderId", 8}, {"originDeviceId", "devA"}});
        nm.handleMessageDeletedNotification(foreignSameDevice);
        QCOMPARE(deletedSpy.count(), 1);
        QCOMPARE(deletedSpy.takeFirst().at(1).toLongLong(), qint64(201));
    }

    // 编辑推送：本端发起设备去重（提前返回，不进入解密/覆写路径）
    void editNotificationDedupsOwnOrigin()
    {
        NetworkManager nm;
        nm.m_userId = 7;
        nm.m_localDeviceId = "devA";
        QSignalSpy editedSpy(&nm, &NetworkManager::messageEdited);

        Packet own;
        own.messageType = MessageType::MessageEditedNotification;
        own.requestId = 0;
        own.payload = payloadBytes(QJsonObject{
            {"messageId", 300}, {"conversationId", 5}, {"editedAt", "t"},
            {"senderId", 7}, {"originDeviceId", "devA"},
            {"content", "ciphertext"}, {"contentType", "e2ee_group"}});
        nm.handleMessageEditedNotification(own);
        // 去重命中：绝不发射（否则会用已推进 ratchet 重试解密并误清空正文）
        QCOMPARE(editedSpy.count(), 0);
    }

    // 私聊编辑队列化：连续编辑均入队不被覆盖；传输槽被占用时泵送不发 socket
    void privateEditsAreQueuedNotClobbered()
    {
        NetworkManager nm;
        nm.m_state = NetworkManager::ConnectionState::Authenticated;
        // 令 fetch 传输槽看似被占（避免 pumpPrivateEditFetch 真实发起请求写 socket）
        nm.m_pendingFetchKeysRequestId = 9999;

        nm.editMessage(5, 9, 100, "first");
        nm.editMessage(5, 9, 101, "second");

        QCOMPARE(nm.m_privateEditQueue.size(), 2);
        QCOMPARE(nm.m_privateEditQueue.head().messageId, qint64(100));
        QCOMPARE(nm.m_privateEditQueue.head().plaintext, "first");
        // 槽忙，泵送未占用编辑标记
        QCOMPARE(nm.m_editFetchInFlight, false);
    }

    // 断线回归（CodeReview P1-1）：自动重连不经 resetAuthState，onDisconnected
    // 必须复位 m_editFetchInFlight 并清空在途编辑/删除容器，否则编辑泵永久堵死；
    // 已入队与已发出未收响应的编辑均上报失败，删除上报失败
    void disconnectClearsInFlightEditState()
    {
        NetworkManager nm;
        nm.m_state = NetworkManager::ConnectionState::Authenticated;
        nm.m_editFetchInFlight = true;
        NetworkManager::PrivateEditWait w;
        w.messageId = 100; w.conversationId = 5; w.peerUserId = 9; w.plaintext = "abc";
        nm.m_privateEditQueue.enqueue(w);
        nm.m_pendingEdits.insert(55, NetworkManager::EditContext{101, 5, "def"});
        nm.m_pendingDeleteRequestIds.insert(66);

        QSignalSpy editFailedSpy(&nm, &NetworkManager::messageEditFailed);
        QSignalSpy deleteFailedSpy(&nm, &NetworkManager::messageDeleteFailed);

        nm.onDisconnected();

        // 在途标记复位、容器清空（否则重连后所有后续私聊编辑静默丢失）
        QCOMPARE(nm.m_editFetchInFlight, false);
        QVERIFY(nm.m_privateEditQueue.isEmpty());
        QVERIFY(nm.m_pendingEdits.isEmpty());
        QVERIFY(nm.m_pendingDeleteRequestIds.isEmpty());
        // 已入队(1) + 已发出未收响应(1) 各上报一次编辑失败；删除上报一次
        QCOMPARE(editFailedSpy.count(), 2);
        QCOMPARE(deleteFailedSpy.count(), 1);
    }

    // 群聊编辑无 Sender Key 时优雅失败（加密空 → messageEditFailed，不入队不发送）
    void groupEditWithoutSenderKeyFailsGracefully()
    {
        NetworkManager nm;
        nm.m_state = NetworkManager::ConnectionState::Authenticated;
        QSignalSpy failedSpy(&nm, &NetworkManager::messageEditFailed);

        nm.editMessage(5, 0, 100, "hello");  // conversationId>0 且 peer=0 → 群聊路径

        QCOMPARE(failedSpy.count(), 1);
        QVERIFY(nm.m_privateEditQueue.isEmpty());
    }

    // 已读游标/会话偏好本地应用：store 未开仍发射刷新信号
    void readCursorAndPrefsEmitSignals()
    {
        NetworkManager nm;
        QSignalSpy cursorSpy(&nm, &NetworkManager::readCursorAdvanced);
        QSignalSpy prefsSpy(&nm, &NetworkManager::conversationPrefsChanged);

        nm.applyReadCursor(5, 200);
        QCOMPARE(cursorSpy.count(), 1);
        QCOMPARE(cursorSpy.takeFirst().at(0).toLongLong(), qint64(5));

        nm.applyConversationPrefs(5, true, false);
        QCOMPARE(prefsSpy.count(), 1);
    }
    // M8.2 P0 回归：文件清单（含 32 字节文件密钥）绝不能进入 QML/JS 引擎。
    // 脱敏必须覆盖所有通向 UI 的路径，而不只是实时推送那一条：
    // 旧实现只处理了 new_message 推送，离线补收、历史翻页、本地缓存回填
    // 三条路径会把含密钥的清单 JSON 当正文渲染进气泡
    void fileManifestNeverReachesUiLayer()
    {
        NetworkManager nm;

        const auto key = FileCrypto::generateFileKey();
        QVERIFY(key.valid);
        FileProtocol::FileManifest manifest;
        manifest.fileId = 77;
        manifest.name = "secret-report.pdf";
        manifest.mime = "application/pdf";
        manifest.chunkSize = FileProtocol::DefaultChunkSize;
        manifest.plainSize = 2 * FileProtocol::plainSizeOfChunk(manifest.chunkSize);
        manifest.cipherSize = manifest.plainSize + 2 * 16;
        manifest.sha256Hex = FileCrypto::sha256Hex("cipher-bytes");
        manifest.key = key.key;
        manifest.iv = key.iv;
        const QString manifestJson = FileProtocol::encodeFileManifest(manifest);
        QVERIFY(!manifestJson.isEmpty());
        const QString keyB64 = QString::fromLatin1(key.key.toBase64());
        QVERIFY(manifestJson.contains(keyB64));  // 清单原文确实带着密钥

        // ① 服务端实时推送形态：带 fileId 字段
        QJsonObject pushed;
        pushed["messageId"] = 1001;
        pushed["fileId"] = 77;
        pushed["content"] = manifestJson;
        pushed["contentType"] = "text";
        nm.sanitizeForUi(pushed);
        QVERIFY(pushed.value("isFileMessage").toBool());
        QVERIFY(pushed.value("content").toString().isEmpty());
        QVERIFY(!pushed.contains("key"));
        QVERIFY(!pushed.contains("iv"));
        QCOMPARE(pushed.value("fileName").toString(), QString("secret-report.pdf"));
        QCOMPARE(pushed.value("fileSizeBytes").toVariant().toLongLong(), manifest.plainSize);
        QCOMPARE(pushed.value("fileId").toVariant().toLongLong(), qint64(77));

        // ② 本地缓存回填形态：LocalStore 的 messages 表无 file_id 列，
        //    消息不带 fileId，必须能从清单里取回并照样脱敏
        QJsonObject fromCache;
        fromCache["messageId"] = 1002;
        fromCache["content"] = manifestJson;
        fromCache["contentType"] = "text";
        nm.sanitizeForUi(fromCache);
        QVERIFY(fromCache.value("isFileMessage").toBool());
        QVERIFY(fromCache.value("content").toString().isEmpty());
        QCOMPARE(fromCache.value("fileId").toVariant().toLongLong(), qint64(77));

        // ③ 密钥确实登记到了 C++ 侧的传输引擎（而不是随正文一起丢掉）：
        //    这是"仅凭 messageId 就能下载/另存"的前提
        QVERIFY(nm.m_fileTransfer != nullptr);
        QVERIFY(nm.m_fileTransfer->m_incoming.contains(1002));
        QCOMPARE(nm.m_fileTransfer->m_incoming.value(1002).key, key.key);
        QCOMPARE(nm.m_fileTransfer->m_incoming.value(1002).iv, key.iv);

        // ④ 批量出口（sync_messages 与本地缓存批量回填）逐条脱敏，
        //    且不得误伤普通文本消息
        QJsonObject plain;
        plain["messageId"] = 1003;
        plain["content"] = "hello world";
        plain["contentType"] = "text";
        QJsonObject offline;
        offline["messageId"] = 1004;
        offline["content"] = manifestJson;
        offline["contentType"] = "text";
        QJsonArray batch;
        batch.append(pushed);
        batch.append(fromCache);
        batch.append(plain);
        batch.append(offline);
        const QJsonArray sanitized = nm.sanitizeArrayForUi(batch);
        QCOMPARE(sanitized.size(), qsizetype(4));
        bool sawPlain = false;
        for (const QJsonValue &value : sanitized) {
            const QJsonObject msg = value.toObject();
            const QString content = msg.value("content").toString();
            QVERIFY2(!content.contains(keyB64), "file key leaked into a UI payload");
            if (msg.value("messageId").toVariant().toLongLong() == 1003) {
                QCOMPARE(content, QString("hello world"));
                QVERIFY(!msg.value("isFileMessage").toBool());
                sawPlain = true;
            } else {
                QVERIFY(content.isEmpty());
            }
        }
        QVERIFY(sawPlain);

        // ⑤ 兜底防线：清单被破坏到 isValid 失败时，只要正文形态像清单
        //    就必须置空，不能因为解析失败而放行原文（宁可少展示一个附件）
        QJsonObject brokenObj = QJsonDocument::fromJson(manifestJson.toUtf8()).object();
        brokenObj["key"] = QString("bm90LWEtdmFsaWQta2V5");
        QJsonObject broken;
        broken["messageId"] = 1005;
        broken["fileId"] = 77;
        broken["content"] = QString::fromUtf8(
            QJsonDocument(brokenObj).toJson(QJsonDocument::Compact));
        nm.sanitizeForUi(broken);
        QVERIFY(broken.value("content").toString().isEmpty());
        QVERIFY(broken.value("isFileMessage").toBool());
        // 非法清单不得被登记（否则下载时才发现解不开）
        QVERIFY(!nm.m_fileTransfer->m_incoming.contains(1005));

        // ⑥ 无法解密的消息（正文是 envelope 密文而非清单）不参与登记，
        //    也不得被误判为文件消息（UI 靠 undecryptable 显示占位）
        QJsonObject undecryptable;
        undecryptable["messageId"] = 1006;
        undecryptable["content"] = QString("{\"type\":\"envelope\",\"v\":1}");
        undecryptable["undecryptable"] = true;
        nm.sanitizeForUi(undecryptable);
        QVERIFY(!undecryptable.value("isFileMessage").toBool());
        QVERIFY(!nm.m_fileTransfer->m_incoming.contains(1006));
    }
};

QTEST_GUILESS_MAIN(TestNetworkManager)
#include "TestNetworkManager.moc"
