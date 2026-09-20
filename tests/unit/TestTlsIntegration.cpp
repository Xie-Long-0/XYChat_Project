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
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUuid>

#include "core/RequestHandler.h"
#include "core/Server.h"

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
