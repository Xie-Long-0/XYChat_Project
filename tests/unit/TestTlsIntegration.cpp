// M5/M8 端到端 TLS 集成测试（主通道）
//
// 覆盖此前无法靠编译保证、也无自动化的一层：真实 Server（真实证书、fail-closed）
// + 真实 QSslSocket 客户端走完整链路（帧编解码 → 重放保护 → 会话鉴权 → DB）。
// 与 TestFileHttpService（数据面 HTTP(S)）互补：本条覆盖主通道；E2EE 报文级链路
// 仍由手动工具 tests/e2e/TestGroupRepro 验证（它需要手工启动 Chat-Server，不进 CTest）。
//
// 端口一律用 0 交 OS 分配：避免与在跑的开发服务抢 12345/12346，也让用例可并行。
// 数据目录经 XYCHAT_SERVER_DATA_DIR 重定向到临时目录（否则 Server 与 handler 会写
// 开发者的真实库）。

#include <QtTest>
#include <QCoreApplication>
#include <QDateTime>
#include <QDeadlineTimer>
#include <QDir>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSslSocket>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUuid>

#include "core/RequestHandler.h"
#include "core/Server.h"
#include "database/DatabaseManager.h"

#include "Packet.h"
#include "PacketCodec.h"
#include "TlsHelper.h"

using namespace XYChat::Protocol;
using namespace XYChat::Security;

namespace {

// 走 TLS 的最小客户端：只做"发一包、等匹配 requestId 的响应"
class TlsTestClient
{
public:
    // caCertPath 为空 = 不注入 CA（用于断言 fail-closed 反例）
    bool connectTo(quint16 port, const QString &caCertPath)
    {
        if (!caCertPath.isEmpty()) {
            const TlsHelper::TlsConfig config = TlsHelper::loadClientConfig(caCertPath);
            if (!config.valid) {
                return false;
            }
            // 与生产客户端（NetworkManager::initTls）逐项一致
            QSslConfiguration sslConfig = QSslConfiguration::defaultConfiguration();
            sslConfig.addCaCertificate(config.caCertificate);
            sslConfig.setProtocol(QSsl::TlsV1_2OrLater);
            sslConfig.setPeerVerifyMode(QSslSocket::VerifyPeer);
            m_socket.setSslConfiguration(sslConfig);
        } else {
            // 不注入 CA：验证模式仍是 VerifyPeer，自签证书必然验不过
            m_socket.setPeerVerifyMode(QSslSocket::VerifyPeer);
        }
        m_socket.connectToHostEncrypted(QHostAddress(QHostAddress::LocalHost).toString(), port);
        return waitEncrypted(5000);
    }

    void close()
    {
        if (m_socket.state() == QAbstractSocket::UnconnectedState) {
            return;
        }
        m_socket.disconnectFromHost();
        const QDeadlineTimer deadline(3000);
        while (!deadline.hasExpired() && m_socket.state() != QAbstractSocket::UnconnectedState) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
    }

    QJsonObject request(MessageType type, const QString &typeName, QJsonObject json)
    {
        json["type"] = typeName;
        if (!m_token.isEmpty()) {
            json["token"] = m_token;
        }
        if (!json.contains("timestamp")) {
            json["timestamp"] = QDateTime::currentSecsSinceEpoch();
        }
        if (!json.contains("nonce")) {
            json["nonce"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
        }

        Packet packet;
        packet.messageType = type;
        packet.requestId = m_nextRequestId++;
        packet.payload = QJsonDocument(json).toJson(QJsonDocument::Compact);
        m_socket.write(PacketCodec::encode(packet));
        m_socket.flush();

        return await(packet.requestId);
    }

    QJsonObject registerUser(const QString &username, const QString &password)
    {
        return request(MessageType::RegisterRequest, "register",
                       { { "username", username }, { "password", password } });
    }

    QJsonObject login(const QString &username, const QString &password, const QString &deviceId)
    {
        const QJsonObject response =
            request(MessageType::LoginRequest, "login",
                    { { "username", username },
                      { "password", password },
                      { "deviceId", deviceId },
                      { "clientVersion", "test" },
                      { "platform", "windows" } });
        m_token = response.value("data").toObject().value("token").toString();
        return response;
    }

    QSslSocket &socket() { return m_socket; }

private:
    // 服务端与客户端在同一个线程：accept 与握手都要靠本线程的事件循环推进，
    // 故不能用 waitForEncrypted（它只服务本 socket 的通知器，服务端永远 accept 不了）
    bool waitEncrypted(int timeoutMs)
    {
        QDeadlineTimer deadline(timeoutMs);
        while (!deadline.hasExpired()) {
            if (m_socket.isEncrypted()) {
                return true;
            }
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            m_socket.waitForReadyRead(10);
        }
        return m_socket.isEncrypted();
    }

    QJsonObject await(quint64 requestId)
    {
        QDeadlineTimer deadline(5000);
        Packet packet;
        while (!deadline.hasExpired()) {
            while (m_codec.nextPacket(packet) == PacketCodec::DecodeStatus::PacketReady) {
                if (packet.requestId == requestId) {
                    return QJsonDocument::fromJson(packet.payload).object();
                }
            }
            if (m_socket.waitForReadyRead(50)) {
                m_codec.appendData(m_socket.readAll());
            }
        }
        qWarning() << "[TestTlsIntegration] timeout waiting for response" << requestId;
        return {};
    }

    // 等待一条不带 requestId 的服务端推送（如 ConversationClearedNotification）
public:
    QJsonObject waitForNotification(MessageType type, int timeoutMs = 5000)
    {
        QDeadlineTimer deadline(timeoutMs);
        Packet packet;
        while (!deadline.hasExpired()) {
            while (m_codec.nextPacket(packet) == PacketCodec::DecodeStatus::PacketReady) {
                if (packet.messageType == type) {
                    return QJsonDocument::fromJson(packet.payload).object();
                }
            }
            // 推送路由经 Server 主线程（队列连接转发到 handler），测试主线程
            // 必须泵事件，否则消息永远停在队列里
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            // 已有可读数据时 waitForReadyRead 会立即返回 false（它只等“新”数据），
            // 必须先看 bytesAvailable，否则缓冲里的推送永远读不出来
            if (m_socket.bytesAvailable() > 0 || m_socket.waitForReadyRead(20)) {
                m_codec.appendData(m_socket.readAll());
            }
        }
        qWarning() << "[TestTlsIntegration] timeout waiting for notification"
                   << static_cast<int>(type);
        return {};
    }

private:
    QSslSocket m_socket;
    PacketCodec m_codec;
    QString m_token;
    quint64 m_nextRequestId = 1;
};

int codeOf(const QJsonObject &response)
{
    return response.value("code").toInt(-1);
}

} // namespace

class TestTlsIntegration : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void serverRefusesPlaintextStartWithoutTls();
    void encryptedRoundTripAcrossTwoUsersAndConnections();
    void idleAuthenticatedConnectionStaysOnline();
    void leaveGroupWithDeleteRecordsCrossDeviceClear();
    void replyToMessageRoundTripOverChannel();
    void replyTargetValidationRejectsBadReferences();
    void clientWithoutCaRejectsHandshake();
    void plaintextClientCannotReachTlsChannel();

private:
    QTemporaryDir *m_dir = nullptr;
    Server *m_server = nullptr;
    quint16 m_port = 0;
    QString m_caPath;
};

void TestTlsIntegration::init()
{
    m_dir = new QTemporaryDir();
    QVERIFY(m_dir->isValid());
    qputenv("XYCHAT_SERVER_DATA_DIR", m_dir->path().toUtf8());

    m_caPath = m_dir->path() + "/certs/ca.crt";
    m_server = new Server();
    // 证书缺失时 initTls 会生成开发证书（CA + 服务端证书）
    QVERIFY(m_server->initTls(m_dir->path() + "/certs"));
    QVERIFY(m_server->tlsEnabled());
    m_server->setStorageRoot(m_dir->path() + "/objects");
    m_server->setFileHttpEndpoint(0, "127.0.0.1");
    QVERIFY(m_server->start(0, false));

    auto *listener = m_server->findChild<ConnectionServer *>();
    QVERIFY(listener != nullptr);
    m_port = listener->serverPort();
    QVERIFY(m_port > 0);
}

void TestTlsIntegration::cleanup()
{
    if (m_server) {
        // 连接断开会让每个 handler 退出事件循环；QThread 在运行中被析构会直接 abort，
        // 故先等各 handler 线程结束再销毁 Server
        const auto handlers = m_server->findChildren<RequestHandler *>();
        for (RequestHandler *handler : handlers) {
            handler->wait(5000);
        }
        delete m_server;
        m_server = nullptr;
    }
    m_port = 0;
    qunsetenv("XYCHAT_SERVER_DATA_DIR");
    delete m_dir;
    m_dir = nullptr;
}

// fail-closed：没有 TLS 又未显式允许明文时，服务端必须拒绝启动（绝不静默降级）
void TestTlsIntegration::serverRefusesPlaintextStartWithoutTls()
{
    Server plainServer;
    QVERIFY(!plainServer.tlsEnabled());
    QVERIFY(!plainServer.start(0, false));
}

// 真实证书 + 真实 TLS：两个连接两个用户的完整链路（帧 → 重放保护 → 会话鉴权 → DB），
// 并验证登录响应下发的数据面基地址就是 Server 实际监听的那个
void TestTlsIntegration::encryptedRoundTripAcrossTwoUsersAndConnections()
{
    TlsTestClient alice;
    QVERIFY(alice.connectTo(m_port, m_caPath));
    QVERIFY(alice.socket().isEncrypted());
    QCOMPARE(codeOf(alice.registerUser("tls_alice", "test123456")), int(ErrorCode::Ok));

    const QJsonObject loginA = alice.login("tls_alice", "test123456", "tls-device-a");
    QCOMPARE(codeOf(loginA), int(ErrorCode::Ok));
    const QJsonObject dataA = loginA.value("data").toObject();
    QVERIFY(!dataA.value("token").toString().isEmpty());
    QVERIFY(!dataA.value("expiresAt").toString().isEmpty());

    // M8.2: 数据面已启动时登录响应必须下发基地址（未启动则不下发，客户端据此禁用文件能力）
    QVERIFY(m_server->fileHttpReady());
    QVERIFY(!m_server->fileTransferBaseUrl().isEmpty());
    QCOMPARE(dataA.value("fileTransferBaseUrl").toString(), m_server->fileTransferBaseUrl());

    // 第二条 TLS 连接、第二个用户：跨连接搜索能命中，证明两连接共享同一套 DB 状态
    TlsTestClient bob;
    QVERIFY(bob.connectTo(m_port, m_caPath));
    QCOMPARE(codeOf(bob.registerUser("tls_bob", "test123456")), int(ErrorCode::Ok));
    QVERIFY(!bob.login("tls_bob", "test123456", "tls-device-b")
                 .value("data")
                 .toObject()
                 .value("token")
                 .toString()
                 .isEmpty());

    const QJsonObject search = alice.request(MessageType::SearchUsersRequest, "search_users",
                                             { { "query", "tls_bob" } });
    QCOMPARE(codeOf(search), int(ErrorCode::Ok));
    const QJsonArray users = search.value("data").toObject().value("users").toArray();
    QCOMPARE(users.size(), 1);
    QCOMPARE(users.at(0).toObject().value("username").toString(), QString("tls_bob"));

    alice.close();
    bob.close();
}

// M11C C2 加固：在线判定必须反映"当前仍有已认证连接"，而不是只看最近活跃时间。
// 把 bob 的 sessions.last_active_at 拨到 1 小时前（等价于"连着但不操作"），
// alice 的 get_conversations 仍应看到 bob 在线；bob 断开后应转为离线。
// 反向断言（断开后为离线）同时覆盖注册表的摘除路径
void TestTlsIntegration::idleAuthenticatedConnectionStaysOnline()
{
    TlsTestClient alice;
    QVERIFY(alice.connectTo(m_port, m_caPath));
    QCOMPARE(codeOf(alice.registerUser("presence_alice", "test123456")), int(ErrorCode::Ok));
    const QJsonObject loginA = alice.login("presence_alice", "test123456", "presence-dev-a");
    QCOMPARE(codeOf(loginA), int(ErrorCode::Ok));
    const qint64 aliceId = loginA.value("data").toObject().value("userId").toVariant().toLongLong();
    QVERIFY(aliceId > 0);

    TlsTestClient bob;
    QVERIFY(bob.connectTo(m_port, m_caPath));
    QCOMPARE(codeOf(bob.registerUser("presence_bob", "test123456")), int(ErrorCode::Ok));
    const QJsonObject loginB = bob.login("presence_bob", "test123456", "presence-dev-b");
    QCOMPARE(codeOf(loginB), int(ErrorCode::Ok));
    const qint64 bobId = loginB.value("data").toObject().value("userId").toVariant().toLongLong();
    QVERIFY(bobId > 0);

    // 建一条私聊会话（在线字段只对私聊回填），并把握手时的活跃时间拨老
    {
        DatabaseManager db("tls_presence_setup");
        QVERIFY(db.initialize());
        QVERIFY(db.getOrCreatePrivateConversation(aliceId, bobId) > 0);
    }
    // 把 bob 的所有 session 活跃时间拨老，排除 120s 启发式兜底的干扰
    auto backdateBobSessions = [this, bobId]() -> bool {
        bool ok = false;
        {
            QSqlDatabase probe = QSqlDatabase::addDatabase("QSQLITE", "tls_presence_probe");
            probe.setDatabaseName(m_dir->path() + "/db/chatapp.db");
            if (!probe.open()) {
                return false;
            }
            QSqlQuery backdate(probe);
            backdate.prepare(
                "UPDATE sessions SET last_active_at = datetime('now','-1 hour') WHERE user_id = ?");
            backdate.addBindValue(bobId);
            ok = backdate.exec();
            probe.close();
        }
        QSqlDatabase::removeDatabase("tls_presence_probe");
        return ok;
    };
    QVERIFY(backdateBobSessions());

    auto bobOnlineForAlice = [&alice, bobId]() -> int {
        const QJsonObject response =
            alice.request(MessageType::GetConversationsRequest, "get_conversations", {});
        if (codeOf(response) != int(ErrorCode::Ok)) {
            return -1;
        }
        const QJsonArray conversations =
            response.value("data").toObject().value("conversations").toArray();
        for (const QJsonValue &value : conversations) {
            const QJsonObject conversation = value.toObject();
            if (conversation.value("peerUserId").toVariant().toLongLong() == bobId) {
                return conversation.value("online").toBool() ? 1 : 0;
            }
        }
        return -1;
    };

    QCOMPARE(bobOnlineForAlice(), 1);

    // 续期会更换 sessionId：注册表必须同步换登记。续期创建的会话活跃时间是
    // "现在"，重新拨老以排除启发式兜底——若旧登记未摘除，断开后它仍会显示在线
    const QJsonObject renewed =
        bob.request(MessageType::TokenRenewRequest, "renew_token", {});
    QCOMPARE(codeOf(renewed), int(ErrorCode::Ok));
    QVERIFY(backdateBobSessions());
    QCOMPARE(bobOnlineForAlice(), 1);

    // 断开 bob：服务端在 handler 线程处理断开并摘除登记，轮询等待其生效。
    // 活跃时间仍是一小时前，故此时只有注册表摘除才能让结果变为离线
    bob.close();
    QDeadlineTimer deadline(5000);
    int online = 1;
    while (!deadline.hasExpired()) {
        online = bobOnlineForAlice();
        if (online == 0) {
            break;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    QCOMPARE(online, 0);

    alice.close();
}

// M13 加固：退出并删除本机记录必须跨设备生效。退群后已非成员，成员态推送
// 无从投递，故服务端向本人账号写 conversation_cleared（携带会话当前最大消息 ID）；
// 本人其他设备据此清空本地并记录水位。无删除意图的退群不写该事件。
// 同时验证 deleteHistory 的形态校验先于任何副作用（非布尔 → InvalidRequest）
void TestTlsIntegration::leaveGroupWithDeleteRecordsCrossDeviceClear()
{
    TlsTestClient alice;
    QVERIFY(alice.connectTo(m_port, m_caPath));
    QCOMPARE(codeOf(alice.registerUser("clear_alice", "test123456")), int(ErrorCode::Ok));
    const QJsonObject loginA =
        alice.login("clear_alice", "test123456", "clear-dev-a");
    QVERIFY(!loginA.value("data").toObject().value("token").toString().isEmpty());
    const qint64 aliceId =
        loginA.value("data").toObject().value("userId").toVariant().toLongLong();
    QVERIFY(aliceId > 0);

    TlsTestClient bob;
    QVERIFY(bob.connectTo(m_port, m_caPath));
    QCOMPARE(codeOf(bob.registerUser("clear_bob", "test123456")), int(ErrorCode::Ok));
    QVERIFY(!bob.login("clear_bob", "test123456", "clear-dev-b")
                 .value("data").toObject().value("token").toString().isEmpty());

    // 本人第二台设备：用于断言“退出并删除记录”的实时推送（不只靠下次登录补拉）
    TlsTestClient aliceSecond;
    QVERIFY(aliceSecond.connectTo(m_port, m_caPath));
    QVERIFY(!aliceSecond.login("clear_alice", "test123456", "clear-dev-a2")
                 .value("data").toObject().value("token").toString().isEmpty());

    // 测试线程同时是 Server 的主线程：登录信号经队列连接回到这里，需泵一次
    // 事件循环把它处理完，否则 messageForUser 路由不到第二台设备
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

    const QJsonObject search = alice.request(MessageType::SearchUsersRequest, "search_users",
                                             { { "query", "clear_bob" } });
    QCOMPARE(codeOf(search), int(ErrorCode::Ok));
    const qint64 bobId = search.value("data").toObject().value("users").toArray()
                             .at(0).toObject().value("userId").toVariant().toLongLong();
    QVERIFY(bobId > 0);

    auto createGroup = [&alice, bobId](const QString &name) -> qint64 {
        const QJsonObject created = alice.request(
            MessageType::CreateGroupRequest, "create_group",
            { { "name", name }, { "memberIds", QJsonArray{ bobId } } });
        if (codeOf(created) != int(ErrorCode::Ok)) {
            return 0;
        }
        return created.value("data").toObject()
                       .value("conversationId").toVariant().toLongLong();
    };

    const qint64 groupDelete = createGroup("clear-group-1");
    QVERIFY(groupDelete > 0);
    const qint64 groupKeep = createGroup("clear-group-2");
    QVERIFY(groupKeep > 0);
    const qint64 groupKick = createGroup("clear-group-3");
    QVERIFY(groupKick > 0);

    // 被踢路径不得写 conversation_cleared（该事件只属于“主动退出并删除记录”）
    const QJsonObject kicked = alice.request(
        MessageType::KickGroupMemberRequest, "kick_group_member",
        { { "conversationId", groupKick }, { "userId", bobId } });
    QCOMPARE(codeOf(kicked), int(ErrorCode::Ok));
    const QJsonObject bobEvents =
        bob.request(MessageType::SyncEventsRequest, "sync_events", { { "afterSeq", 0 } });
    QCOMPARE(codeOf(bobEvents), int(ErrorCode::Ok));
    for (const QJsonValue &value :
         bobEvents.value("data").toObject().value("events").toArray()) {
        const QJsonObject event = value.toObject();
        if (event.value("type").toString() != "conversation_cleared") {
            continue;
        }
        QVERIFY(event.value("payload").toObject()
                    .value("conversationId").toVariant().toLongLong() != groupKick);
    }

    const QJsonObject sent = alice.request(MessageType::SendMessageRequest, "send_message",
                                           { { "conversationId", groupDelete },
                                             { "content", "history to be deleted" },
                                             { "contentType", "text" },
                                             { "clientMessageId", "clear-cm-1" } });
    QCOMPARE(codeOf(sent), int(ErrorCode::Ok));
    const qint64 watermarkSource = sent.value("data").toObject()
                                        .value("messageId").toVariant().toLongLong();
    QVERIFY(watermarkSource > 0);

    // 形态校验：deleteHistory 不是布尔 → 拒绝且不产生副作用（仍可在随后合法退群）
    const QJsonObject malformed = alice.request(
        MessageType::LeaveGroupRequest, "leave_group",
        { { "conversationId", groupDelete }, { "deleteHistory", "yes" } });
    QCOMPARE(codeOf(malformed), int(ErrorCode::InvalidRequest));

    // 退群前会话确实在列表中（避免“列表为空也通过”的空洞断言）
    auto conversationPresent = [&alice](qint64 convId) -> bool {
        const QJsonObject response = alice.request(
            MessageType::GetConversationsRequest, "get_conversations", {});
        if (codeOf(response) != int(ErrorCode::Ok)) {
            return false;
        }
        for (const QJsonValue &value :
             response.value("data").toObject().value("conversations").toArray()) {
            if (value.toObject().value("conversationId").toVariant().toLongLong() == convId) {
                return true;
            }
        }
        return false;
    };
    QVERIFY(conversationPresent(groupDelete));

    // 推送路由依赖 Server 主线程把两台的登录信号（队列连接）处理完；
    // 测试线程即主线程，平时由事件循环推进。按 session 数等待——onlineUserIds
    // 只到用户粒度，无法区分第二台设备是否已登记
    QDeadlineTimer routeDeadline(2000);
    while (!routeDeadline.hasExpired() && m_server->onlineSessionCount() < 3) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    // alice 两台设备 + bob 一条连接
    QCOMPARE(m_server->onlineSessionCount(), 3);

    const QJsonObject leftWithDelete = alice.request(
        MessageType::LeaveGroupRequest, "leave_group",
        { { "conversationId", groupDelete }, { "deleteHistory", true } });
    QCOMPARE(codeOf(leftWithDelete), int(ErrorCode::Ok));

    // 本人第二台设备应立即收到 ConversationClearedNotification（实时推送），
    // 且发起设备 ID 可用于其自行忽略
    const QJsonObject liveClear =
        aliceSecond.waitForNotification(MessageType::ConversationClearedNotification);
    QCOMPARE(liveClear.value("conversationId").toVariant().toLongLong(), groupDelete);
    QCOMPARE(liveClear.value("originDeviceId").toString(), QString("clear-dev-a"));
    QVERIFY(liveClear.value("clearedBeforeMessageId").toVariant().toLongLong()
            >= watermarkSource);

    // 本人的账号事件流必须出现 conversation_cleared，水位不低于删除前的最后消息
    const QJsonObject syncAfterDelete =
        alice.request(MessageType::SyncEventsRequest, "sync_events", { { "afterSeq", 0 } });
    QCOMPARE(codeOf(syncAfterDelete), int(ErrorCode::Ok));
    const QJsonArray events =
        syncAfterDelete.value("data").toObject().value("events").toArray();
    qint64 clearedSeq = 0;
    qint64 clearedWatermark = -1;
    for (const QJsonValue &value : events) {
        const QJsonObject event = value.toObject();
        if (event.value("type").toString() != "conversation_cleared") {
            continue;
        }
        const QJsonObject payload = event.value("payload").toObject();
        if (payload.value("conversationId").toVariant().toLongLong() == groupDelete) {
            clearedSeq = event.value("seq").toVariant().toLongLong();
            clearedWatermark = payload.value("clearedBeforeMessageId").toVariant().toLongLong();
        }
    }
    QVERIFY2(clearedSeq > 0, "缺少 conversation_cleared 事件");
    // 水位是退群时刻该会话的最大消息 ID：群主退群会先写一条 owner_transferred
    // 系统消息，故可能大于删除前的最后一条普通消息；断言只要求覆盖它
    QVERIFY(clearedWatermark >= watermarkSource);

    // 退群后会话不再出现在本人列表中
    QVERIFY(!conversationPresent(groupDelete));

    // 无删除意图的退群不写 conversation_cleared（保留历史由本机自行决定）
    const QJsonObject leftKeep = alice.request(
        MessageType::LeaveGroupRequest, "leave_group", { { "conversationId", groupKeep } });
    QCOMPARE(codeOf(leftKeep), int(ErrorCode::Ok));
    const QJsonObject syncAfterKeep =
        alice.request(MessageType::SyncEventsRequest, "sync_events", { { "afterSeq", clearedSeq } });
    QCOMPARE(codeOf(syncAfterKeep), int(ErrorCode::Ok));
    for (const QJsonValue &value :
         syncAfterKeep.value("data").toObject().value("events").toArray()) {
        const QJsonObject event = value.toObject();
        if (event.value("type").toString() != "conversation_cleared") {
            continue;
        }
        QVERIFY(event.value("payload").toObject()
                    .value("conversationId").toVariant().toLongLong() != groupKeep);
    }

    alice.close();
    aliceSecond.close();
    bob.close();
}

// M11B B2：引用（回复）全链路。群路径可用明文 text 构造（私聊路径要求合法
// E2EE envelope，需预密钥体系，故协议面校验由群路径代表）：
// 解析形态 → 校验目标属本会话 → 落库 → sync_messages 回读引用字段
void TestTlsIntegration::replyToMessageRoundTripOverChannel()
{
    TlsTestClient alice;
    QVERIFY(alice.connectTo(m_port, m_caPath));
    QCOMPARE(codeOf(alice.registerUser("reply_alice", "test123456")), int(ErrorCode::Ok));
    QVERIFY(!alice.login("reply_alice", "test123456", "reply-dev-a")
                 .value("data").toObject().value("token").toString().isEmpty());

    TlsTestClient bob;
    QVERIFY(bob.connectTo(m_port, m_caPath));
    QCOMPARE(codeOf(bob.registerUser("reply_bob", "test123456")), int(ErrorCode::Ok));
    QVERIFY(!bob.login("reply_bob", "test123456", "reply-dev-b")
                 .value("data").toObject().value("token").toString().isEmpty());

    const QJsonObject search = alice.request(MessageType::SearchUsersRequest, "search_users",
                                             { { "query", "reply_bob" } });
    QCOMPARE(codeOf(search), int(ErrorCode::Ok));
    const QJsonArray users = search.value("data").toObject().value("users").toArray();
    QCOMPARE(users.size(), 1);
    const qint64 bobId = users.at(0).toObject().value("userId").toVariant().toLongLong();
    QVERIFY(bobId > 0);

    const QJsonObject created = alice.request(MessageType::CreateGroupRequest, "create_group",
                                              { { "name", "reply-group" },
                                                { "memberIds", QJsonArray{ bobId } } });
    QCOMPARE(codeOf(created), int(ErrorCode::Ok));
    const qint64 convId = created.value("data").toObject()
                                 .value("conversationId").toVariant().toLongLong();
    QVERIFY(convId > 0);

    const QJsonObject first = alice.request(MessageType::SendMessageRequest, "send_message",
                                            { { "conversationId", convId },
                                              { "content", "original message" },
                                              { "contentType", "text" },
                                              { "clientMessageId", "reply-cm-1" } });
    QCOMPARE(codeOf(first), int(ErrorCode::Ok));
    const qint64 firstId = first.value("data").toObject()
                                .value("messageId").toVariant().toLongLong();
    QVERIFY(firstId > 0);

    // 带引用的发送：响应回带 replyToMessageId
    const QJsonObject reply = alice.request(MessageType::SendMessageRequest, "send_message",
                                            { { "conversationId", convId },
                                              { "content", "a reply" },
                                              { "contentType", "text" },
                                              { "clientMessageId", "reply-cm-2" },
                                              { "replyToMessageId", firstId } });
    QCOMPARE(codeOf(reply), int(ErrorCode::Ok));
    const QJsonObject replyData = reply.value("data").toObject();
    QCOMPARE(replyData.value("replyToMessageId").toVariant().toLongLong(), firstId);
    const qint64 replyId = replyData.value("messageId").toVariant().toLongLong();
    QVERIFY(replyId > firstId);

    // 接收方（另一连接、另一账号）经 sync_messages 拿到引用字段：
    // 只回 ID 与被引用者名，正文摘要不在服务端（仍为密文/原文由客户端渲染）
    const QJsonObject synced = bob.request(MessageType::SyncMessagesRequest, "sync_messages",
                                           { { "conversationId", convId },
                                             { "afterId", 0 },
                                             { "limit", 50 } });
    QCOMPARE(codeOf(synced), int(ErrorCode::Ok));
    const QJsonArray messages = synced.value("data").toObject().value("messages").toArray();
    bool seenOriginal = false;
    bool seenReply = false;
    for (const QJsonValue &value : messages) {
        const QJsonObject msg = value.toObject();
        const qint64 id = msg.value("messageId").toVariant().toLongLong();
        if (id == firstId) {
            seenOriginal = true;
            QVERIFY(!msg.contains("replyToMessageId"));
        } else if (id == replyId) {
            seenReply = true;
            QCOMPARE(msg.value("replyToMessageId").toVariant().toLongLong(), firstId);
            QCOMPARE(msg.value("replyToSender").toString(), QString("reply_alice"));
            // 不允许服务端把被引用消息的正文塞进引用字段（E2EE 下它本就是密文，
            // 一旦有"摘要"字段就意味着服务端在拼明文）
            QVERIFY(!msg.contains("replyPreview"));
        }
    }
    QVERIFY(seenOriginal);
    QVERIFY(seenReply);

    alice.close();
    bob.close();
}

// M11B B2：非法引用一律拒绝——形态错误（字符串/负数/小数）与语义错误
//（不存在、跨会话）都回 InvalidRequest，且不落库（校验在写库之前）
void TestTlsIntegration::replyTargetValidationRejectsBadReferences()
{
    TlsTestClient alice;
    QVERIFY(alice.connectTo(m_port, m_caPath));
    QCOMPARE(codeOf(alice.registerUser("reply_val_alice", "test123456")), int(ErrorCode::Ok));
    QVERIFY(!alice.login("reply_val_alice", "test123456", "val-dev-a")
                 .value("data").toObject().value("token").toString().isEmpty());

    TlsTestClient bob;
    QVERIFY(bob.connectTo(m_port, m_caPath));
    QCOMPARE(codeOf(bob.registerUser("reply_val_bob", "test123456")), int(ErrorCode::Ok));
    QVERIFY(!bob.login("reply_val_bob", "test123456", "val-dev-b")
                 .value("data").toObject().value("token").toString().isEmpty());

    const QJsonObject search = bob.request(MessageType::SearchUsersRequest, "search_users",
                                           { { "query", "reply_val_alice" } });
    const QJsonArray users = search.value("data").toObject().value("users").toArray();
    QCOMPARE(users.size(), 1);
    const qint64 aliceId = users.at(0).toObject().value("userId").toVariant().toLongLong();

    const QJsonObject created = bob.request(MessageType::CreateGroupRequest, "create_group",
                                            { { "name", "reply-val-group" },
                                              { "memberIds", QJsonArray{ aliceId } } });
    QCOMPARE(codeOf(created), int(ErrorCode::Ok));
    const qint64 convId = created.value("data").toObject()
                                 .value("conversationId").toVariant().toLongLong();

    const QJsonObject inGroup = bob.request(MessageType::SendMessageRequest, "send_message",
                                            { { "conversationId", convId },
                                              { "content", "target" },
                                              { "contentType", "text" },
                                              { "clientMessageId", "reply-val-1" } });
    QCOMPARE(codeOf(inGroup), int(ErrorCode::Ok));
    const qint64 inGroupId = inGroup.value("data").toObject()
                                    .value("messageId").toVariant().toLongLong();

    // 另一个群（bob 自己开的）里的消息：跨会话引用必须被拒
    const QJsonObject other = bob.request(MessageType::CreateGroupRequest, "create_group",
                                          { { "name", "reply-val-other" },
                                            { "memberIds", QJsonArray{} } });
    QCOMPARE(codeOf(other), int(ErrorCode::Ok));
    const qint64 otherConvId = other.value("data").toObject()
                                     .value("conversationId").toVariant().toLongLong();
    QVERIFY(otherConvId != convId);
    const QJsonObject inOther = bob.request(MessageType::SendMessageRequest, "send_message",
                                            { { "conversationId", otherConvId },
                                              { "content", "message in another group" },
                                              { "contentType", "text" },
                                              { "clientMessageId", "reply-val-other-1" } });
    QCOMPARE(codeOf(inOther), int(ErrorCode::Ok));
    const qint64 inOtherId = inOther.value("data").toObject()
                                    .value("messageId").toVariant().toLongLong();
    QVERIFY(inOtherId > 0 && inOtherId != inGroupId);

    auto sendWith = [&](const QString &key, const QJsonValue &replyValue) {
        return bob.request(MessageType::SendMessageRequest, "send_message",
                           { { "conversationId", convId },
                             { "content", "bad reply" },
                             { "contentType", "text" },
                             { "clientMessageId", key },
                             { "replyToMessageId", replyValue } });
    };

    // 形态错误
    QCOMPARE(codeOf(sendWith("reply-val-str", QString("1"))), int(ErrorCode::InvalidRequest));
    QCOMPARE(codeOf(sendWith("reply-val-neg", -3)), int(ErrorCode::InvalidRequest));
    QCOMPARE(codeOf(sendWith("reply-val-frac", 1.5)), int(ErrorCode::InvalidRequest));
    // 语义错误：不存在 / 属于别的会话
    QCOMPARE(codeOf(sendWith("reply-val-missing", 999999)), int(ErrorCode::InvalidRequest));
    QCOMPARE(codeOf(sendWith("reply-val-cross", inOtherId)), int(ErrorCode::InvalidRequest));

    // 被拒的请求都不得落库：群里只应有那一条目标消息（+ 建群系统消息）
    const QJsonObject synced = bob.request(MessageType::SyncMessagesRequest, "sync_messages",
                                           { { "conversationId", convId },
                                             { "afterId", 0 },
                                             { "limit", 50 } });
    QCOMPARE(codeOf(synced), int(ErrorCode::Ok));
    const QJsonArray messages = synced.value("data").toObject().value("messages").toArray();
    for (const QJsonValue &value : messages) {
        const QJsonObject msg = value.toObject();
        if (msg.value("contentType").toString() == QLatin1String("text")) {
            QCOMPARE(msg.value("messageId").toVariant().toLongLong(), inGroupId);
        }
    }

    alice.close();
    bob.close();
}

// fail-closed：客户端拿不到 CA 时必须握手失败，不能"验不了也照样连"
void TestTlsIntegration::clientWithoutCaRejectsHandshake()
{
    TlsTestClient client;
    QVERIFY(!client.connectTo(m_port, QString()));
    QVERIFY(!client.socket().isEncrypted());
    client.close();
}

// TLS 端口把明文当作坏掉的 ClientHello：既不得被升级成明文会话，也解不出任何响应
void TestTlsIntegration::plaintextClientCannotReachTlsChannel()
{
    QTcpSocket plain;
    plain.connectToHost(QHostAddress(QHostAddress::LocalHost).toString(), m_port);
    QVERIFY(plain.waitForConnected(3000));

    QJsonObject json;
    json["type"] = "register";
    json["username"] = "tls_eve";
    json["password"] = "test123456";
    json["timestamp"] = QDateTime::currentSecsSinceEpoch();
    json["nonce"] = QUuid::createUuid().toString(QUuid::WithoutBraces);

    Packet packet;
    packet.messageType = MessageType::RegisterRequest;
    packet.requestId = 1;
    packet.payload = QJsonDocument(json).toJson(QJsonDocument::Compact);
    plain.write(PacketCodec::encode(packet));
    plain.flush();

    QByteArray received;
    QDeadlineTimer deadline(1500);
    while (!deadline.hasExpired()) {
        if (plain.waitForReadyRead(100)) {
            received += plain.readAll();
        }
    }

    PacketCodec codec;
    codec.appendData(received);
    Packet decoded;
    QVERIFY(codec.nextPacket(decoded) != PacketCodec::DecodeStatus::PacketReady);
    plain.disconnectFromHost();
}

QTEST_GUILESS_MAIN(TestTlsIntegration)
#include "TestTlsIntegration.moc"
