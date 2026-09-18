#include <QtTest>
#include <QSignalSpy>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <QTimeZone>
#include <QUuid>

#include "NetworkManager.h"
#include "LocalStore.h"
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

        // ④ 逐条脱敏（消息页泵对每一条消息都调用同一出口），
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
        bool sawPlain = false;
        for (const QJsonValue &value : batch) {
            QJsonObject msg = value.toObject();
            nm.sanitizeForUi(msg);
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

    // M10：“正在输入”推送透传——conversationId>0 时发射 typingReceived（携发起者信息），
    // 畸形推送（conversationId<=0）忽略不发射
    void typingNotificationEmitsSignal()
    {
        NetworkManager nm;
        QSignalSpy typingSpy(&nm, &NetworkManager::typingReceived);

        Packet ok;
        ok.messageType = MessageType::TypingNotification;
        ok.requestId = 0;
        ok.payload = payloadBytes(QJsonObject{
            {"conversationId", 5}, {"userId", 9},
            {"username", "bob"}, {"typing", true}});
        nm.handleTypingNotification(ok);
        QCOMPARE(typingSpy.count(), 1);
        const QList<QVariant> args = typingSpy.takeFirst();
        QCOMPARE(args.at(0).toLongLong(), qint64(5));   // conversationId
        QCOMPARE(args.at(1).toLongLong(), qint64(9));   // userId
        QCOMPARE(args.at(2).toString(), "bob");         // username
        QCOMPARE(args.at(3).toBool(), true);            // typing

        // 畸形推送：conversationId<=0 忽略
        Packet bad;
        bad.messageType = MessageType::TypingNotification;
        bad.requestId = 0;
        bad.payload = payloadBytes(QJsonObject{{"conversationId", 0}, {"userId", 9}});
        nm.handleTypingNotification(bad);
        QCOMPARE(typingSpy.count(), 0);
    }

    // M10：sendTyping 客户端节流与前置校验（三条路径均在 sendPacket 前返回，无需 socket）：
    // ① 未认证：直接返回，不记录节流时间戳
    // ② conversationId<=0：直接返回
    // ③ 窗口内（<4s）重复 typing=true：提前返回，时间戳不变（证明未进入发送路径）
    void sendTypingGuardsAndThrottle()
    {
        NetworkManager anon;
        anon.sendTyping(5, true);
        QVERIFY(anon.m_lastTypingSentMs.isEmpty());

        NetworkManager nm;
        nm.m_state = NetworkManager::ConnectionState::Authenticated;
        nm.sendTyping(0, true);                       // 非法会话 ID
        QVERIFY(!nm.m_lastTypingSentMs.contains(0));

        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        nm.m_lastTypingSentMs[5] = nowMs;             // 预置“刚发送过”
        nm.sendTyping(5, true);                       // 窗口内 → 提前返回
        QCOMPARE(nm.m_lastTypingSentMs[5], nowMs);    // 时间戳未变
    }

    // M10：会话删除响应——requestId 命中才处理；Ok 发射 conversationDeleted 并清在途，
    // 非 Ok 发射 conversationDeleteFailed（LocalStore 未开，缓存清理分支被安全跳过）。
    // 在途请求为 requestId→conversationId 多槽映射：并发双删除时后发请求不覆盖前一个，
    // 两个响应乱序到达均能各自命中（P1 回归锁定：单值实现会丢失首个删除且不自愈）
    void deleteConversationResponseMatchesPending()
    {
        NetworkManager nm;
        // 并发两个在途删除：33→会话5、44→会话6（后发的 44 不得覆盖 33）
        nm.m_pendingDeleteConversationRequests.insert(33, 5);
        nm.m_pendingDeleteConversationRequests.insert(44, 6);
        QSignalSpy deletedSpy(&nm, &NetworkManager::conversationDeleted);
        QSignalSpy failedSpy(&nm, &NetworkManager::conversationDeleteFailed);

        // 不匹配的 requestId：忽略，两个在途均未消费
        Packet mismatch;
        mismatch.messageType = MessageType::DeleteConversationResponse;
        mismatch.requestId = 99;
        QJsonObject mj;
        mj["code"] = static_cast<int>(ErrorCode::Ok);
        mj["data"] = QJsonObject{{"conversationId", 5}};
        mismatch.payload = payloadBytes(mj);
        nm.handleDeleteConversationResponse(mismatch);
        QCOMPARE(deletedSpy.count(), 0);
        QCOMPARE(nm.m_pendingDeleteConversationRequests.size(), 2);

        // 先回后发的 44（乱序）：命中并消费 44，33 仍在途（未被覆盖丢失）
        Packet ok44;
        ok44.messageType = MessageType::DeleteConversationResponse;
        ok44.requestId = 44;
        QJsonObject oj44;
        oj44["code"] = static_cast<int>(ErrorCode::Ok);
        oj44["data"] = QJsonObject{{"conversationId", 6}};
        ok44.payload = payloadBytes(oj44);
        nm.handleDeleteConversationResponse(ok44);
        QCOMPARE(deletedSpy.count(), 1);
        QCOMPARE(deletedSpy.takeFirst().at(0).toLongLong(), qint64(6));
        QVERIFY(!nm.m_pendingDeleteConversationRequests.contains(44));
        QVERIFY(nm.m_pendingDeleteConversationRequests.contains(33));

        // 再回先发的 33：命中并消费；响应 data 故意不带 conversationId，
        // 验证回退到在途登记值（不依赖服务端回传）
        Packet ok33;
        ok33.messageType = MessageType::DeleteConversationResponse;
        ok33.requestId = 33;
        QJsonObject oj33;
        oj33["code"] = static_cast<int>(ErrorCode::Ok);
        oj33["data"] = QJsonObject{};
        ok33.payload = payloadBytes(oj33);
        nm.handleDeleteConversationResponse(ok33);
        QCOMPARE(deletedSpy.count(), 1);
        QCOMPARE(deletedSpy.takeFirst().at(0).toLongLong(), qint64(5));
        QVERIFY(nm.m_pendingDeleteConversationRequests.isEmpty());

        // 错误码：发射 conversationDeleteFailed 并消费在途
        nm.m_pendingDeleteConversationRequests.insert(55, 7);
        Packet err;
        err.messageType = MessageType::DeleteConversationResponse;
        err.requestId = 55;
        QJsonObject ej;
        ej["code"] = static_cast<int>(ErrorCode::PermissionDenied);
        ej["message"] = "Only the group owner can delete";
        err.payload = payloadBytes(ej);
        nm.handleDeleteConversationResponse(err);
        QCOMPARE(failedSpy.count(), 1);
        QVERIFY(!nm.m_pendingDeleteConversationRequests.contains(55));
    }

    // M10：会话删除推送——发起设备（operatorId+originDeviceId 同时命中）去重，
    // 他人（不同 operatorId）正常发射 conversationDeleted；畸形推送忽略
    void conversationDeletedNotificationDedupsOwnOrigin()
    {
        NetworkManager nm;
        nm.m_userId = 7;
        nm.m_localDeviceId = "devA";
        QSignalSpy deletedSpy(&nm, &NetworkManager::conversationDeleted);

        Packet own;
        own.messageType = MessageType::ConversationDeletedNotification;
        own.requestId = 0;
        own.payload = payloadBytes(QJsonObject{
            {"conversationId", 5}, {"operatorId", 7}, {"originDeviceId", "devA"}});
        nm.handleConversationDeletedNotification(own);
        QCOMPARE(deletedSpy.count(), 0);  // 本端其他设备发起，去重

        // 同机不同账号（deviceId 相同但 operatorId 不同）不得被误去重
        Packet foreignSameDevice;
        foreignSameDevice.messageType = MessageType::ConversationDeletedNotification;
        foreignSameDevice.requestId = 0;
        foreignSameDevice.payload = payloadBytes(QJsonObject{
            {"conversationId", 6}, {"operatorId", 8}, {"originDeviceId", "devA"}});
        nm.handleConversationDeletedNotification(foreignSameDevice);
        QCOMPARE(deletedSpy.count(), 1);
        QCOMPARE(deletedSpy.takeFirst().at(0).toLongLong(), qint64(6));

        // 畸形推送：conversationId<=0 忽略
        Packet bad;
        bad.messageType = MessageType::ConversationDeletedNotification;
        bad.requestId = 0;
        bad.payload = payloadBytes(QJsonObject{{"conversationId", 0}, {"operatorId", 8}});
        nm.handleConversationDeletedNotification(bad);
        QCOMPARE(deletedSpy.count(), 0);
    }

    // M12：sync_messages 响应改为多槽匹配 + 时间片泵异步 emit。
    // ① 单槽实现下后发请求会覆盖先发，先发的页响应到达即被静默丢弃；
    // ② 整页处理必须推迟到时间片泵（handler 返回时尚未 emit）
    void syncMessagesResponsesRoutedByMultiSlotAndEmittedAsync()
    {
        NetworkManager nm;
        QSignalSpy syncedSpy(&nm, &NetworkManager::messagesSynced);

        const auto makeResponse = [](quint64 requestId, bool hasMore) {
            Packet packet;
            packet.messageType = MessageType::SyncMessagesResponse;
            packet.requestId = requestId;
            QJsonArray messages;
            QJsonObject msg;
            msg["messageId"] = 11;
            msg["conversationId"] = 100;
            msg["senderId"] = 7;
            msg["senderUsername"] = "peer";
            // 存量明文（非 envelope）：解密路径原样通过，测试无需密钥
            msg["content"] = "hello";
            msg["contentType"] = "text";
            msg["status"] = "delivered";
            msg["createdAt"] = "2026-09-05T12:00:00Z";
            messages.append(msg);
            QJsonObject data;
            data["conversationId"] = 100;
            data["hasMore"] = hasMore;
            data["messages"] = messages;
            QJsonObject json;
            json["code"] = static_cast<int>(ErrorCode::Ok);
            json["data"] = data;
            packet.payload = payloadBytes(json);
            return packet;
        };

        // 两个在途请求（切会话）：单槽会丢掉第一个
        nm.m_pendingSyncRequests.insert(1, 100);
        nm.m_pendingSyncRequests.insert(2, 200);
        nm.handleSyncMessagesResponse(makeResponse(1, false));
        nm.handleSyncMessagesResponse(makeResponse(2, true));

        // 未登记的 requestId：忽略且不影响在途登记
        Packet stray = makeResponse(3, false);
        nm.handleSyncMessagesResponse(stray);
        QCOMPARE(nm.m_pendingSyncRequests.size(), 0);

        // 异步：handler 已返回，页面推进交给时间片泵
        QCOMPARE(syncedSpy.count(), 0);
        QTRY_COMPARE(syncedSpy.count(), 2);
        QList<QVariant> first = syncedSpy.at(0);
        QCOMPARE(first.at(0).toLongLong(), qint64(100));  // conversationId
        QCOMPARE(first.at(1).toJsonArray().size(), 1);
        QCOMPARE(first.at(1).toJsonArray().at(0).toObject().value("content").toString(),
                 QString("hello"));
        QCOMPARE(first.at(2).toBool(), false);            // hasMore
        QList<QVariant> second = syncedSpy.at(1);
        QCOMPARE(second.at(0).toLongLong(), qint64(200));
        QCOMPARE(second.at(2).toBool(), true);
    }

    // M12：整页（大页需跨多个时间片）只 emit 一次、消息保持原序——
    // 若按片 emit，QML 侧会把后半页当成新到的一页重复处理
    void largePageEmittedOnceInOrderAcrossSlices()
    {
        NetworkManager nm;
        QSignalSpy syncedSpy(&nm, &NetworkManager::messagesSynced);

        NetworkManager::MessagePageJob job;
        job.conversationId = 77;
        job.fromServer = true;
        job.hasMore = true;
        const int kCount = 300;
        for (int i = 0; i < kCount; ++i) {
            QJsonObject msg;
            msg["messageId"] = 1000 + i;
            msg["conversationId"] = 77;
            msg["senderId"] = 7;
            msg["senderUsername"] = "peer";
            msg["content"] = QString(2048, QLatin1Char('x'));
            msg["contentType"] = "text";
            msg["status"] = "delivered";
            msg["createdAt"] = "2026-09-05T12:00:00Z";
            job.messages.append(msg);
        }
        nm.enqueueMessagePage(job);

        QCOMPARE(syncedSpy.count(), 0);
        // 显式放宽预算：QTRY_COMPARE 默认 5000 ms，而本用例刻意用 300 条 × 2 KB
        // 的"大页"来逼出跨时间片续做（分片预算 kMessagePageSliceNs 是编译期常量、
        // 无注入点，只能靠规模把它撑到多片）。Debug 构建下泵稳定需要约 14.7 s，
        // 默认预算会在任何逻辑断言求值之前先超时——那是预算问题而非正确性缺陷，
        // 故不得据此判定本用例失败。60 s 留约 4 倍余量，同时仍能抓住真回归
        //（泵若卡死则 60 s 内恒不 emit）
        QTRY_COMPARE_WITH_TIMEOUT(syncedSpy.count(), 1, 60000);
        const QList<QVariant> args = syncedSpy.at(0);
        QCOMPARE(args.at(0).toLongLong(), qint64(77));
        QCOMPARE(args.at(2).toBool(), true);
        const QJsonArray emitted = args.at(1).toJsonArray();
        QCOMPARE(emitted.size(), kCount);
        QCOMPARE(emitted.first().toObject().value("messageId").toVariant().toLongLong(),
                 qint64(1000));
        QCOMPARE(emitted.last().toObject().value("messageId").toVariant().toLongLong(),
                 qint64(1000 + kCount - 1));
        QVERIFY(!nm.m_messagePageActive);
        QVERIFY(nm.m_messagePageQueue.isEmpty());
    }

    // M12：缓存页（切会话时先展示本地缓存）的解密与脱敏同样在时间片泵内完成，
    // 且不落库、不崩溃（本地库未打开）。取行阶段正文以 contentCipher 交给泵
    //（正常路径见 TestLocalStore::batchUpsertAndRowDecryptMatchSinglePath）；
    // 这里锁住兜底语义：行内没有密文时绝不放行任何正文
    void cachePageNeverLeaksContentWithoutCipher()
    {
        NetworkManager nm;
        QSignalSpy syncedSpy(&nm, &NetworkManager::messagesSynced);

        NetworkManager::MessagePageJob cacheJob;
        cacheJob.conversationId = 42;
        QJsonObject cached;
        cached["messageId"] = 1;
        cached["conversationId"] = 42;
        cached["senderId"] = 7;
        cached["content"] = "must not pass through";
        cached["contentType"] = "text";
        cacheJob.messages.append(cached);
        nm.enqueueMessagePage(cacheJob);

        // 异步：入队不立即 emit，由泵在下一轮事件循环推进
        QCOMPARE(syncedSpy.count(), 0);
        QTRY_COMPARE(syncedSpy.count(), 1);
        const QList<QVariant> args = syncedSpy.at(0);
        QCOMPARE(args.at(0).toLongLong(), qint64(42));
        QCOMPARE(args.at(2).toBool(), false);
        const QJsonObject emitted = args.at(1).toJsonArray().at(0).toObject();
        QVERIFY(emitted.value("content").toString().isEmpty());
        QVERIFY(emitted.value("undecryptable").toBool());
        QVERIFY(!nm.m_messagePageActive);
        QVERIFY(nm.m_messagePageQueue.isEmpty());
    }

    // M9/M12 欠账修复：在途请求超时清扫。断线/登出会清空登记项，但"服务端漏答而
    // 连接未断"没有回收路径——登记项会残留到下次登出（编辑/删除还会一直挂在 UI 的
    // 乐观态上）。这里锁住：到期即清退并复用失败信号上报，未到期的必须原样保留
    void inFlightRequestsTimeOutAndReportFailures()
    {
        NetworkManager nm;
        QSignalSpy editSpy(&nm, &NetworkManager::messageEditFailed);
        QSignalSpy deleteSpy(&nm, &NetworkManager::messageDeleteFailed);
        QSignalSpy convSpy(&nm, &NetworkManager::conversationDeleteFailed);

        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        // 四类登记表各一条，全部已到期
        nm.m_pendingEdits.insert(101, NetworkManager::EditContext{1, 2, "x"});
        nm.m_pendingDeadlines.insert(101, now - 1);
        nm.m_pendingDeleteRequestIds.insert(102);
        nm.m_pendingDeadlines.insert(102, now - 1);
        nm.m_pendingDeleteConversationRequests.insert(103, 9);
        nm.m_pendingDeadlines.insert(103, now - 1);
        nm.m_pendingSyncRequests.insert(104, 7);
        nm.m_pendingDeadlines.insert(104, now - 1);
        // 一条尚未到期的编辑：不得被误清（否则用户刚发出的编辑会被立刻判失败）
        nm.m_pendingEdits.insert(105, NetworkManager::EditContext{2, 2, "y"});
        nm.m_pendingDeadlines.insert(105, now + 60000);

        nm.sweepInFlightRequests();

        QCOMPARE(editSpy.count(), 1);
        QCOMPARE(editSpy.at(0).at(0).toString(), QString("Request timed out"));
        QCOMPARE(deleteSpy.count(), 1);
        QCOMPARE(convSpy.count(), 1);
        QVERIFY(!nm.m_pendingEdits.contains(101));
        QVERIFY(!nm.m_pendingDeleteRequestIds.contains(102));
        QVERIFY(!nm.m_pendingDeleteConversationRequests.contains(103));
        QVERIFY(!nm.m_pendingSyncRequests.contains(104));
        // 未到期的原样保留，且其截止登记仍在（否则它会永远等不到清扫）
        QVERIFY(nm.m_pendingEdits.contains(105));
        QVERIFY(nm.m_pendingDeadlines.contains(105));
    }

    // 正常响应到达时必须撤销截止登记：否则表只增不减，定时器永不停止空转唤醒
    void inFlightDeadlineIsClearedOnResponse()
    {
        NetworkManager nm;
        // 惰性创建：无在途请求时不应存在定时器，更不能空转
        QVERIFY(!nm.m_inFlightSweepTimer);

        nm.trackInFlightRequest(201);
        QVERIFY(nm.m_inFlightSweepTimer);
        QVERIFY(nm.m_inFlightSweepTimer->isActive());
        QVERIFY(nm.m_pendingDeadlines.contains(201));

        nm.m_pendingEdits.insert(201, NetworkManager::EditContext{100, 5, "newtext"});
        Packet p;
        p.messageType = MessageType::EditMessageResponse;
        p.requestId = 201;
        QJsonObject json;
        json["code"] = static_cast<int>(ErrorCode::Ok);
        json["data"] = QJsonObject{{"conversationId", 5}, {"editedAt", "2026-09-05T12:00:00Z"}};
        p.payload = payloadBytes(json);
        nm.handleEditMessageResponse(p);

        QVERIFY(!nm.m_pendingDeadlines.contains(201));   // 登记随之作废
        QVERIFY(!nm.m_inFlightSweepTimer->isActive());   // 表空即停表
    }

    // M12：消息页队列必须封顶。每页含 ≤100 行密文与已解密正文，无上限时快速连点
    // 大量会话会把它们全留在内存；丢最旧的（该会话下次激活会重新请求该页）
    void messagePageQueueIsBounded()
    {
        NetworkManager nm;
        const int kOver = 5;
        for (int i = 0; i < NetworkManager::MaxQueuedMessagePages + kOver; ++i) {
            NetworkManager::MessagePageJob job;
            job.conversationId = 100 + i;
            nm.enqueueMessagePage(job);
        }
        // 入队后不跑事件循环：泵尚未推进，此刻看到的就是队列上限本身
        QCOMPARE(nm.m_messagePageQueue.size(),
                 static_cast<qsizetype>(NetworkManager::MaxQueuedMessagePages));
        QCOMPARE(nm.m_messagePageQueue.head().conversationId, qint64(100 + kOver));
        QCOMPARE(nm.m_messagePageQueue.last().conversationId,
                 qint64(100 + NetworkManager::MaxQueuedMessagePages + kOver - 1));
        for (const NetworkManager::MessagePageJob &job : nm.m_messagePageQueue) {
            QVERIFY(job.conversationId >= 100 + kOver);
        }
    }

    // M9 欠账修复（续）：私聊编辑等 fetch_keys 传输槽时，若响应漏答而连接未断，
    // m_editFetchInFlight 恒真会让队列里所有后续编辑静默停摆到下次重连。
    // 必须出队上报卡住的那条并解除槽位占用，使队列继续推进
    void privateEditFetchTimeoutUnsticksQueue()
    {
        NetworkManager nm;
        nm.m_privateEditQueue.enqueue(NetworkManager::PrivateEditWait{1, 2, 42, "p"});
        nm.m_privateEditQueue.enqueue(NetworkManager::PrivateEditWait{3, 4, 42, "q"});
        nm.m_editFetchInFlight = true;
        nm.m_privateEditFetchDeadlineMs = QDateTime::currentMSecsSinceEpoch() - 1;
        nm.m_pendingFetchKeysRequestId = 777;
        nm.m_fetchKeysTargetUserId = 42;

        QSignalSpy failSpy(&nm, &NetworkManager::messageEditFailed);
        nm.sweepInFlightRequests();

        QCOMPARE(failSpy.count(), 1);
        QCOMPARE(failSpy.at(0).at(0).toString(), QString("Request timed out"));
        // 只清掉卡住的队首：后面那条仍排队等待，不得被一起丢弃
        QCOMPARE(nm.m_privateEditQueue.size(), qsizetype(1));
        QCOMPARE(nm.m_privateEditQueue.head().messageId, qint64(3));
        // 续泵：队列未空 -> 立刻为新队首重新占用槽位（新 requestId，绝不沿用旧的
        // 777，否则晚到的旧响应会被误认成本次请求的应答）
        QVERIFY(nm.m_editFetchInFlight);
        QVERIFY(nm.m_pendingFetchKeysRequestId != 0);
        QVERIFY(nm.m_pendingFetchKeysRequestId != 777);
    }

    // P2 欠账修复：Sender-Key 分发在途时该群必须 fail-closed。原实现"先落盘后分发"
    // 会把未分发的新 key 立刻投入使用，分发一旦被永久拒绝（超大群超长等），
    // 本端就产出一批谁也解不开的群消息；反过来若继续用旧 key，轮换期内新成员
    // 读不到、被移除成员仍能读。故在途期间任何既有密钥都不得使用
    void groupSenderKeyBlockedWhileDistributionPending()
    {
        NetworkManager nm;
        nm.m_userId = 7;
        nm.m_localDeviceId = "dev0";
        // 先放一把"可用"的旧 key，证明在途分发连它也不能用
        nm.m_groupSenderKeys.insert(5,
            XYChat::Security::GroupE2eeCrypto::generateSenderKey());

        XYChat::Security::GroupE2eeCrypto::SenderKey out;
        QVERIFY(nm.ensureGroupSenderKey(5, out));
        QVERIFY(out.valid);

        NetworkManager::PendingSenderKeyDistribution pending;
        pending.key = XYChat::Security::GroupE2eeCrypto::generateSenderKey();
        pending.chunkRequestIds.insert("chunk-a", 9001);
        nm.m_pendingSenderKeyDist.insert(5, pending);

        out = XYChat::Security::GroupE2eeCrypto::SenderKey{};
        QVERIFY(!nm.ensureGroupSenderKey(5, out));
        QVERIFY(!out.valid);
        // 不得用旧 key 顶替：密钥整体不可用（调用方据此把消息留在 outbox）
        QVERIFY(nm.encryptGroupMessage(5, "hello").isEmpty());
    }

    // P2 欠账修复：新 key 必须等到**全部**分片 ACK 才启用。部分成功若照样启用，
    // 未收到分片的成员解不开消息，等于退回被修复的缺陷
    void groupSenderKeyActivatedOnlyAfterAllChunksAcked()
    {
        NetworkManager nm;
        nm.m_userId = 7;
        nm.m_localDeviceId = "dev0";

        const auto newKey = XYChat::Security::GroupE2eeCrypto::generateSenderKey();
        NetworkManager::PendingSenderKeyDistribution pending;
        pending.key = newKey;
        pending.chunkRequestIds.insert("chunk-a", 9001);
        pending.chunkRequestIds.insert("chunk-b", 9002);
        nm.m_pendingSenderKeyDist.insert(5, pending);
        nm.m_pendingDistChunkConv.insert("chunk-a", 5);
        nm.m_pendingDistChunkConv.insert("chunk-b", 5);
        nm.m_pendingDistRequestConv.insert(9001, 5);
        nm.m_pendingDistRequestConv.insert(9002, 5);
        nm.m_pendingSendByRequestId.insert(9001, "chunk-a");
        nm.m_pendingSendByRequestId.insert(9002, "chunk-b");

        const auto ack = [](quint64 requestId) {
            Packet p;
            p.messageType = MessageType::SendMessageResponse;
            p.requestId = requestId;
            p.payload = payloadBytes(QJsonObject{{"code", static_cast<int>(ErrorCode::Ok)}});
            return p;
        };

        // 第一片确认：仍不可用，且不得落盘/进内存缓存
        nm.handleSendMessageResponse(ack(9001));
        QVERIFY(nm.m_pendingSenderKeyDist.contains(5));
        QVERIFY(!nm.m_groupSenderKeys.contains(5));
        XYChat::Security::GroupE2eeCrypto::SenderKey out;
        QVERIFY(!nm.ensureGroupSenderKey(5, out));

        // 最后一片确认：新 key 才生效
        nm.handleSendMessageResponse(ack(9002));
        QVERIFY(!nm.m_pendingSenderKeyDist.contains(5));
        QVERIFY(nm.m_groupSenderKeys.contains(5));
        QCOMPARE(nm.m_groupSenderKeys.value(5).keyId, newKey.keyId);
        QVERIFY(nm.ensureGroupSenderKey(5, out));
        QCOMPARE(out.keyId, newKey.keyId);
        QVERIFY(!nm.encryptGroupMessage(5, "hello").isEmpty());
    }

    // P2 欠账修复：任一被拒的分片作废整次分发（不是把该片丢掉、其余照用），
    // 并清退其余分片的在途登记，使晚到的 ACK 不会被当成普通消息回显
    void groupSenderKeyDiscardedWhenChunkRejected()
    {
        NetworkManager nm;
        nm.m_userId = 7;
        nm.m_localDeviceId = "dev0";

        NetworkManager::PendingSenderKeyDistribution pending;
        pending.key = XYChat::Security::GroupE2eeCrypto::generateSenderKey();
        pending.chunkRequestIds.insert("chunk-a", 9001);
        pending.chunkRequestIds.insert("chunk-b", 9002);
        nm.m_pendingSenderKeyDist.insert(5, pending);
        nm.m_pendingDistChunkConv.insert("chunk-a", 5);
        nm.m_pendingDistChunkConv.insert("chunk-b", 5);
        nm.m_pendingDistRequestConv.insert(9001, 5);
        nm.m_pendingDistRequestConv.insert(9002, 5);
        nm.m_pendingSendByRequestId.insert(9001, "chunk-a");
        nm.m_pendingSendByRequestId.insert(9002, "chunk-b");

        QSignalSpy sentSpy(&nm, &NetworkManager::messageSent);

        // chunk-b 被服务端拒（例如正文超长）
        Packet rejected;
        rejected.messageType = MessageType::SendMessageResponse;
        rejected.requestId = 9002;
        rejected.payload = payloadBytes(QJsonObject{
            {"code", static_cast<int>(ErrorCode::InvalidRequest)},
            {"message", "Message content too long"}});
        nm.handleSendMessageResponse(rejected);

        QVERIFY(!nm.m_pendingSenderKeyDist.contains(5));
        QVERIFY(!nm.m_groupSenderKeys.contains(5));
        QVERIFY(nm.m_pendingDistRequestConv.isEmpty());
        // 已 ACK 的 chunk-a 的登记也必须清退，否则它的复核 ACK 会走进
        // "普通消息"分支并凭空 emit 一条 messageSent
        QVERIFY(!nm.m_pendingSendByRequestId.contains(9001));
        QVERIFY(!nm.m_pendingDistChunkConv.contains("chunk-a"));
        // 退避：避免立刻重拉空耗 fetch 预算
        QVERIFY(nm.m_groupDistBackoffUntil.value(5, 0)
                > QDateTime::currentSecsSinceEpoch());

        Packet lateAck;
        lateAck.messageType = MessageType::SendMessageResponse;
        lateAck.requestId = 9001;
        lateAck.payload = payloadBytes(QJsonObject{{"code", static_cast<int>(ErrorCode::Ok)}});
        nm.handleSendMessageResponse(lateAck);
        QCOMPARE(sentSpy.count(), 0);
    }

    // P2 欠账修复：分片漏答（丢包）必须由在途超时清扫清退。不清理的话分发永久
    // 挂起，而挂起期间该群一律发不出消息——一次丢包就把这个群卡到下次重连
    void groupSenderKeyDistributionTimeoutDiscardsPending()
    {
        NetworkManager nm;
        nm.m_userId = 7;
        nm.m_localDeviceId = "dev0";

        NetworkManager::PendingSenderKeyDistribution pending;
        pending.key = XYChat::Security::GroupE2eeCrypto::generateSenderKey();
        pending.chunkRequestIds.insert("chunk-a", 9001);
        nm.m_pendingSenderKeyDist.insert(5, pending);
        nm.m_pendingDistChunkConv.insert("chunk-a", 5);
        nm.m_pendingDistRequestConv.insert(9001, 5);
        nm.m_pendingSendByRequestId.insert(9001, "chunk-a");
        nm.m_pendingDeadlines.insert(9001, QDateTime::currentMSecsSinceEpoch() - 1);

        nm.sweepInFlightRequests();

        QVERIFY(nm.m_pendingSenderKeyDist.isEmpty());
        QVERIFY(nm.m_pendingDistRequestConv.isEmpty());
        QVERIFY(!nm.m_pendingSendByRequestId.contains(9001));
        XYChat::Security::GroupE2eeCrypto::SenderKey out;
        QVERIFY(!nm.ensureGroupSenderKey(5, out));
    }

    // P2 欠账修复：分发在途或刚失败处于退避期时，flushOutbox 不得为本群再发起
    // fetch_group_keys（否则一次分发失败会被立刻重拉，空耗限流预算并放大预密钥消耗）
    void flushOutboxSuppressesGroupFetchWhilePendingOrBackoff()
    {
        NetworkManager nm;
        nm.m_userId = 7;
        nm.m_localDeviceId = "dev0";
        nm.m_e2eeReady = true;
        nm.setState(NetworkManager::ConnectionState::Authenticated);

        NetworkManager::OutboxItem item;
        item.clientMessageId = "m1";
        item.conversationId = 5;
        item.content = "hello";
        nm.m_outbox.append(item);

        // 正例对照：既无在途分发也无退避 → 必须发起拉取。缺了它，下面的"零"
        // 可能只是恰好没走到拉取分支，用例就抓不住回归
        nm.flushOutbox();
        QVERIFY(nm.m_pendingFetchGroupKeysRequestId != 0);

        // 在途分发 → 不再拉取（分发完成后由 handleSendMessageResponse 再次 flush）
        nm.m_pendingFetchGroupKeysRequestId = 0;
        NetworkManager::PendingSenderKeyDistribution pending;
        pending.key = XYChat::Security::GroupE2eeCrypto::generateSenderKey();
        pending.chunkRequestIds.insert("chunk-a", 9001);
        nm.m_pendingSenderKeyDist.insert(5, pending);
        nm.flushOutbox();
        QCOMPARE(nm.m_pendingFetchGroupKeysRequestId, quint64(0));

        // 退避期内 → 同样不拉取（到期由重试定时器兜底）
        nm.m_pendingSenderKeyDist.clear();
        nm.m_groupDistBackoffUntil.insert(5, QDateTime::currentSecsSinceEpoch() + 30);
        nm.flushOutbox();
        QCOMPARE(nm.m_pendingFetchGroupKeysRequestId, quint64(0));
    }

    // P2 欠账修复（续）：分发被切成多条分片后，同一 keyId 的分发消息会到达多次
    //（历史同步亦可能重投）。而 decodeDistribution 不回传 iteration（恒 0），
    // 无条件 saveSenderKey 会把接收端已推进的 ratchet 打回 0——轻则重放时从零
    // 重新 ratchet，重则迭代数超过 MaxRatchetSteps(2000) 后该发送方后续消息永久
    // 不可解。本用例锁住"链密钥一致即幂等跳过、链密钥不一致仍照旧覆盖"的语义
    void repeatedDistributionInstallKeepsLocalRatchet()
    {
        namespace Sec = XYChat::Security;
        using Sec::E2eeCrypto;
        using Sec::GroupE2eeCrypto;
        const QString user =
            "dist_" + QUuid::createUuid().toString(QUuid::Id128).left(12).toLower();

        NetworkManager nm;
        nm.m_userId = 7;
        nm.m_username = user;
        nm.m_localDeviceId = "dev0";
        nm.m_identityKey = E2eeCrypto::generateX25519KeyPair();
        QVERIFY(nm.m_identityKey.valid);
        QVERIFY(nm.m_localStore.open(user, "dev0"));

        // 构造"仅本机设备"的分发消息：chainKey 以本机身份密钥自加密
        //（prekeyId = SelfCopyPrekeyId，接收端无需预密钥即可解出）
        const auto makeDistribution = [&nm](const QByteArray &chainKey, const QString &keyId,
                                            const QByteArray &publicSigningKey) {
            const auto eph = XYChat::Security::E2eeCrypto::generateX25519KeyPair();
            const QByteArray dh = XYChat::Security::E2eeCrypto::ecdh(
                eph.privateKey, nm.m_identityKey.publicKey);
            const QByteArray shared = dh + dh;
            const QByteArray messageKey =
                XYChat::Security::E2eeCrypto::deriveMessageKey(shared);
            const auto gcm =
                XYChat::Security::E2eeCrypto::aesGcmEncrypt(messageKey, chainKey.toBase64());

            GroupE2eeCrypto::DistributionEntry entry;
            entry.userId = nm.m_userId;
            entry.deviceId = nm.m_localDeviceId;
            entry.envelope.deviceId = nm.m_localDeviceId;
            entry.envelope.prekeyId = XYChat::Security::E2eeCrypto::SelfCopyPrekeyId;
            entry.envelope.ephemeralPublicKey = eph.publicKey;
            entry.envelope.iv = gcm.iv;
            entry.envelope.ciphertext = gcm.ciphertext;

            GroupE2eeCrypto::SenderKey key;
            key.keyId = keyId;
            key.publicSigningKey = publicSigningKey;

            QJsonObject msg;
            msg["conversationId"] = qint64(9);
            msg["content"] = QString::fromUtf8(QJsonDocument(GroupE2eeCrypto::encodeDistribution(
                9, nm.m_userId, nm.m_localDeviceId, key, {entry}))
                .toJson(QJsonDocument::Compact));
            return msg;
        };

        const QByteArray chainKeyA = E2eeCrypto::generateRandomBytes(32);
        QCOMPARE(chainKeyA.size(), 32);
        const auto signing = GroupE2eeCrypto::generateSenderKey();
        const QString keyId = signing.keyId;
        const QByteArray signingPub = signing.publicSigningKey;

        // 首次安装
        QVERIFY(nm.processGroupSenderKeyDistribution(
            makeDistribution(chainKeyA, keyId, signingPub)));

        QByteArray stored;
        int iteration = -1;
        const auto load = [&nm, &keyId, &stored, &iteration]() {
            QByteArray publicKey;
            QByteArray privateKey;
            return nm.m_localStore.loadSenderKey(9, 7, "dev0", keyId, stored, publicKey,
                                                 privateKey, iteration);
        };
        QVERIFY(load());
        QCOMPARE(stored, chainKeyA);
        QCOMPARE(iteration, 0);

        // 对照：同 keyId 但链密钥不同，而本地这把 key 还没被用过（iteration == 0）
        // → 仍按"后到者覆盖"写入。证明上面的保护只针对**已用过**的 key，
        // 不是"同 keyId 一律跳过"
        const QByteArray chainKeyB = E2eeCrypto::generateRandomBytes(32);
        QVERIFY(nm.processGroupSenderKeyDistribution(
            makeDistribution(chainKeyB, keyId, signingPub)));
        QVERIFY(load());
        QCOMPARE(stored, chainKeyB);
        QCOMPARE(iteration, 0);

        // 模拟接收端已用这把 key 推进过 ratchet
        const QByteArray advanced = GroupE2eeCrypto::ratchetChainKey(chainKeyB);
        QVERIFY(nm.m_localStore.saveSenderKey(9, 7, "dev0", keyId, advanced, signingPub,
                                              QByteArray(), 5));

        // 同一分片重投（含首片 chainKeyA）：本 key 已用过 → 跳过安装，
        // 已推进的 ratchet 与链密钥都不得被打回
        QVERIFY(nm.processGroupSenderKeyDistribution(
            makeDistribution(chainKeyA, keyId, signingPub)));
        QVERIFY(load());
        QCOMPARE(iteration, 5);
        QCOMPARE(stored, advanced);

        nm.m_localStore.closeAndDestroy();
    }
};

QTEST_GUILESS_MAIN(TestNetworkManager)
#include "TestNetworkManager.moc"
