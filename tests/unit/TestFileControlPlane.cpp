// M8 文件控制面 handler 级测试（协议 90-99 五个处理器）
//
// 覆盖矩阵里此前的空白：TestDatabaseManager 覆盖数据层、TestFileHttpService 与
// TestFileTransfer 覆盖数据面/客户端引擎，而 RequestHandler 的五个文件处理器
// （入参校验顺序、限流窗口、幂等、finalize 结果分类、枚举预言机合并）只有人工复核。
//
// 做法：进程内起真实回环连接 + 真实 RequestHandler（明文分支：桌面测试无需证书，
// 也避免与 TLS 用例重复），客户端侧用 PacketCodec 手工发包并断言响应码与 data。
// 数据目录经 XYCHAT_SERVER_DATA_DIR 重定向到临时目录（否则 handler 会写开发者的
// 真实库——它的连接名按 this 指针生成，无法注入 :memory: 连接），对象存储指向
// 同一临时目录下的子目录。

#include <QtTest>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDeadlineTimer>
#include <QDir>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUuid>

#include "core/NonceCache.h"
#include "core/RequestHandler.h"
#include "core/Server.h"
#include "storage/LocalFileStorage.h"

#include "FileProtocol.h"
#include "Packet.h"
#include "PacketCodec.h"

using namespace XYChat::Protocol;
using namespace XYChat::Server;

namespace {

// 分片口径：64 KiB（MinChunkSize）+ 100 字节尾片。两片即可覆盖"非末片恒等于
// chunkSize、末片为余量"的约定，又不必造大文件
constexpr qint64 kChunkSize = 64 * 1024;
constexpr qint64 kTailBytes = 100;
constexpr int kChunkCount = 2;

QByteArray deterministicBytes(qint64 size, quint8 seed)
{
    QByteArray out;
    out.resize(static_cast<int>(size));
    for (int i = 0; i < out.size(); ++i) {
        out[i] = static_cast<char>((seed + i * 31) & 0xFF);
    }
    return out;
}

} // namespace

class TestFileControlPlane : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void replayProtectionPrecedesAuth();
    void createValidatesParamsThenIssuesTicket();
    void createEnforcesQuotaThenRateWindow();
    void queryReflectsReceivedChunks();
    void queryMergesNotExistAndNotYours();
    void completeReportsIncompleteWithReceivedIndexes();
    void completeRejectsDataFaultAndMarksFailed();
    void completeIsIdempotentAndEnablesDownloadTicket();
    void cancelIsIdempotentAndBlocksComplete();

private:
    // 原始发包：调用方自己组装字段（测重放保护/免鉴权路径时用）
    QJsonObject requestRaw(MessageType type, const QString &typeName, QJsonObject json);
    // 业务请求：自动补 timestamp/nonce/token
    QJsonObject request(MessageType type, const QString &typeName, const QJsonObject &fields = {});
    QJsonObject awaitResponse(quint64 requestId);

    QJsonObject createUpload(qint64 sizeBytes, qint64 chunkSize, int chunkCount,
                             const QString &sha256Hex);
    // create 响应刻意不回传 blobKey（存储键不出服务端），测试自己查库取回
    QString blobKeyFor(qint64 fileId);
    qint64 jsonToId(const QJsonValue &value) const;
    int codeOf(const QJsonObject &response) const;

    QTemporaryDir *m_dir = nullptr;
    LocalFileStorage *m_storage = nullptr;
    NonceCache m_nonceCache;
    ConnectionServer *m_server = nullptr;
    RequestHandler *m_handler = nullptr;
    QTcpSocket *m_socket = nullptr;
    PacketCodec m_codec;
    QHash<quint64, QJsonObject> m_stashed;
    quint64 m_nextRequestId = 1;
    QString m_token;
    qint64 m_userId = 0;
    QByteArray m_chunk0;
    QByteArray m_chunk1;
    QString m_sha256;
};

void TestFileControlPlane::init()
{
    m_dir = new QTemporaryDir();
    QVERIFY(m_dir->isValid());
    // 必须在 handler 建库之前设置：它的数据目录是编译期常量，只有这个环境变量能重定向
    qputenv("XYCHAT_SERVER_DATA_DIR", m_dir->path().toUtf8());

    m_storage = new LocalFileStorage(m_dir->path() + "/objects");
    QVERIFY(m_storage->initialize());

    m_chunk0 = deterministicBytes(kChunkSize, 7);
    m_chunk1 = deterministicBytes(kTailBytes, 200);
    m_sha256 = QString::fromLatin1(
        QCryptographicHash::hash(m_chunk0 + m_chunk1, QCryptographicHash::Sha256).toHex());

    // 与生产同一条 accept 路径：ConnectionServer 只把描述符交出来，由 handler 接管
    m_server = new ConnectionServer();
    connect(m_server, &ConnectionServer::socketAccepted, this, [this](qintptr descriptor) {
        m_handler = new RequestHandler(descriptor);
        m_handler->setNonceCache(&m_nonceCache);
        m_handler->setObjectStorage(m_storage);
        // 不设 ssl 配置 = 明文分支（RequestHandler::m_tlsEnabled 默认 false）
        m_handler->start();
    });
    QVERIFY(m_server->listen(QHostAddress::LocalHost, 0));

    m_socket = new QTcpSocket();
    m_socket->connectToHost(QHostAddress::LocalHost, m_server->serverPort());
    QVERIFY(m_socket->waitForConnected(5000));
    // accept 发生在服务端的 notifier 上，需要事件循环推进才会构造 handler
    QTRY_VERIFY_WITH_TIMEOUT(m_handler != nullptr, 5000);

    const QJsonObject reg = request(MessageType::RegisterRequest, "register",
                                    { { "username", "cp_alice" }, { "password", "test123456" } });
    QCOMPARE(codeOf(reg), int(ErrorCode::Ok));

    const QJsonObject login = request(MessageType::LoginRequest, "login",
                                      { { "username", "cp_alice" },
                                        { "password", "test123456" },
                                        { "deviceId", "cp-device" },
                                        { "clientVersion", "test" },
                                        { "platform", "windows" } });
    QCOMPARE(codeOf(login), int(ErrorCode::Ok));
    const QJsonObject loginData = login.value("data").toObject();
    m_token = loginData.value("token").toString();
    m_userId = jsonToId(loginData.value("userId"));
    QVERIFY(!m_token.isEmpty());
    QVERIFY(m_userId > 0);
}

void TestFileControlPlane::cleanup()
{
    if (m_socket) {
        if (m_socket->state() != QAbstractSocket::UnconnectedState) {
            m_socket->disconnectFromHost();
            m_socket->waitForDisconnected(3000);
        }
    }
    if (m_handler) {
        // 断连会让 handler 退出事件循环；等线程真正结束再销毁，否则 QThread 析构会中止运行中的线程
        if (!m_handler->wait(5000)) {
            m_handler->quit();
            m_handler->wait(2000);
        }
        delete m_handler;
        m_handler = nullptr;
    }
    delete m_socket;
    m_socket = nullptr;
    delete m_server;
    m_server = nullptr;
    delete m_storage;
    m_storage = nullptr;
    m_stashed.clear();
    m_token.clear();
    m_nextRequestId = 1;
    // 先卸环境变量再删临时目录，避免后续用例（或残留线程）拿到已删除的路径
    qunsetenv("XYCHAT_SERVER_DATA_DIR");
    delete m_dir;
    m_dir = nullptr;
}

QJsonObject TestFileControlPlane::requestRaw(MessageType type, const QString &typeName,
                                             QJsonObject json)
{
    json["type"] = typeName;

    Packet packet;
    packet.messageType = type;
    packet.requestId = m_nextRequestId++;
    packet.payload = QJsonDocument(json).toJson(QJsonDocument::Compact);
    m_socket->write(PacketCodec::encode(packet));
    m_socket->flush();

    return awaitResponse(packet.requestId);
}

QJsonObject TestFileControlPlane::request(MessageType type, const QString &typeName,
                                          const QJsonObject &fields)
{
    QJsonObject json = fields;
    json["timestamp"] = QDateTime::currentSecsSinceEpoch();
    json["nonce"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!m_token.isEmpty()) {
        json["token"] = m_token;
    }
    return requestRaw(type, typeName, json);
}

QJsonObject TestFileControlPlane::awaitResponse(quint64 requestId)
{
    const auto stashed = m_stashed.find(requestId);
    if (stashed != m_stashed.end()) {
        return stashed.value();
    }

    QDeadlineTimer deadline(5000);
    Packet packet;
    while (!deadline.hasExpired()) {
        while (m_codec.nextPacket(packet) == PacketCodec::DecodeStatus::PacketReady) {
            const QJsonObject payload = QJsonDocument::fromJson(packet.payload).object();
            if (packet.requestId == requestId) {
                return payload;
            }
            m_stashed.insert(packet.requestId, payload);
        }
        if (m_socket->waitForReadyRead(50)) {
            m_codec.appendData(m_socket->readAll());
        }
    }
    qWarning() << "[TestFileControlPlane] timeout waiting for response" << requestId;
    return {};
}

int TestFileControlPlane::codeOf(const QJsonObject &response) const
{
    return response.value("code").toInt(-1);
}

qint64 TestFileControlPlane::jsonToId(const QJsonValue &value) const
{
    return value.toVariant().toLongLong();
}

QJsonObject TestFileControlPlane::createUpload(qint64 sizeBytes, qint64 chunkSize,
                                               int chunkCount, const QString &sha256Hex)
{
    return request(MessageType::FileUploadCreateRequest, "file_upload_create",
                   { { "sizeBytes", sizeBytes },
                     { "chunkSize", chunkSize },
                     { "chunkCount", chunkCount },
                     { "sha256", sha256Hex } });
}

QString TestFileControlPlane::blobKeyFor(qint64 fileId)
{
    QString blobKey;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "cp_inspect");
        db.setDatabaseName(m_dir->path() + "/db/chatapp.db");
        if (db.open()) {
            QSqlQuery q(db);
            q.prepare("SELECT blob_key FROM files WHERE id = ?");
            q.addBindValue(fileId);
            if (q.exec() && q.next()) {
                blobKey = q.value(0).toString();
            }
        }
        db.close();
    }
    QSqlDatabase::removeDatabase("cp_inspect");
    return blobKey;
}

// 重放保护先于鉴权：两条前置关卡各自独立生效，畸形/未认证请求都到不了 handler
void TestFileControlPlane::replayProtectionPrecedesAuth()
{
    const QJsonObject bare = requestRaw(MessageType::FileUploadCreateRequest,
                                        "file_upload_create",
                                        { { "sizeBytes", kChunkSize + kTailBytes },
                                          { "chunkSize", kChunkSize },
                                          { "chunkCount", kChunkCount },
                                          { "sha256", m_sha256 } });
    QCOMPARE(codeOf(bare), int(ErrorCode::ReplayRejected));

    // 带重放字段但不带 token：pipeline 要求逐包验会话
    QJsonObject json;
    json["timestamp"] = QDateTime::currentSecsSinceEpoch();
    json["nonce"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
    json["fileId"] = 1;
    const QJsonObject noToken = requestRaw(MessageType::FileUploadQueryRequest,
                                           "file_upload_query", json);
    QCOMPARE(codeOf(noToken), int(ErrorCode::SessionInvalid));
}

// 入参形态先于限流与配额消费；合法请求才签发票据，且不回传存储键
void TestFileControlPlane::createValidatesParamsThenIssuesTicket()
{
    QCOMPARE(codeOf(createUpload(kChunkSize + kTailBytes, kChunkSize, kChunkCount, "not-hex-64")),
             int(ErrorCode::InvalidRequest));

    // chunkCount 与 ceil(密文体积 / chunkSize) 不符：少报可用分片数绕过上限
    QCOMPARE(codeOf(createUpload(kChunkSize + kTailBytes, kChunkSize, kChunkCount + 1, m_sha256)),
             int(ErrorCode::InvalidRequest));

    QVERIFY(codeOf(createUpload(MaxFileSize + 1, DefaultChunkSize,
                                static_cast<int>(chunkCountFor(MaxFileSize + 1, DefaultChunkSize)),
                                m_sha256))
            == int(ErrorCode::FileTooLarge));

    const QJsonObject ok = createUpload(kChunkSize + kTailBytes, kChunkSize, kChunkCount, m_sha256);
    QCOMPARE(codeOf(ok), int(ErrorCode::Ok));
    const QJsonObject data = ok.value("data").toObject();
    QVERIFY(jsonToId(data.value("fileId")) > 0);
    QVERIFY(!data.value("uploadTicket").toString().isEmpty());
    QCOMPARE(data.value("expiresInSeconds").toInt(), UploadTicketTtlSeconds);
    QVERIFY(!data.contains("blobKey"));
}

// 并发上传配额（8）与限流窗口（20/60s）是两道独立闸门：配额先撞，额度耗尽后改扭限流
void TestFileControlPlane::createEnforcesQuotaThenRateWindow()
{
    int okCount = 0;
    int quotaCount = 0;
    bool rateLimited = false;
    QSet<int> unexpected;

    for (int i = 0; i < 30 && !rateLimited; ++i) {
        const int code = codeOf(createUpload(kChunkSize + kTailBytes, kChunkSize,
                                             kChunkCount, m_sha256));
        if (code == int(ErrorCode::Ok)) {
            ++okCount;
        } else if (code == int(ErrorCode::FileQuotaExceeded)) {
            ++quotaCount;
        } else if (code == int(ErrorCode::RateLimited)) {
            rateLimited = true;
        } else {
            unexpected.insert(code);
        }
    }

    QVERIFY2(unexpected.isEmpty(), qPrintable(QString("unexpected codes: %1")
                                                  .arg([&unexpected]() {
                                                      QStringList parts;
                                                      for (int c : unexpected) {
                                                          parts << QString::number(c);
                                                      }
                                                      return parts.join(',');
                                                  }())));
    QVERIFY(rateLimited);
    QCOMPARE(okCount, MaxConcurrentUploadsPerUser);
    QVERIFY(quotaCount > 0);
}

void TestFileControlPlane::queryReflectsReceivedChunks()
{
    const QJsonObject created = createUpload(kChunkSize + kTailBytes, kChunkSize,
                                             kChunkCount, m_sha256);
    QCOMPARE(codeOf(created), int(ErrorCode::Ok));
    const qint64 fileId = jsonToId(created.value("data").toObject().value("fileId"));

    const QJsonObject emptyQuery = request(MessageType::FileUploadQueryRequest,
                                           "file_upload_query", { { "fileId", fileId } });
    QCOMPARE(codeOf(emptyQuery), int(ErrorCode::Ok));
    const QJsonObject emptyData = emptyQuery.value("data").toObject();
    QCOMPARE(emptyData.value("status").toString(), QString(FileStatus::Uploading));
    QCOMPARE(emptyData.value("chunkCount").toInt(), kChunkCount);
    QCOMPARE(emptyData.value("receivedChunks").toArray().size(), 0);

    const QString blobKey = blobKeyFor(fileId);
    QVERIFY(!blobKey.isEmpty());
    QVERIFY(m_storage->putChunk(blobKey, 0, m_chunk0));

    const QJsonObject partial = request(MessageType::FileUploadQueryRequest,
                                        "file_upload_query", { { "fileId", fileId } });
    QCOMPARE(codeOf(partial), int(ErrorCode::Ok));
    const QJsonArray received = partial.value("data").toObject().value("receivedChunks").toArray();
    QCOMPARE(received.size(), 1);
    QCOMPARE(received.at(0).toInt(), 0);
}

// 枚举预言机：上传控制面只服务本人，"不存在"与"不是你的"必须回同一个错误码
void TestFileControlPlane::queryMergesNotExistAndNotYours()
{
    const QJsonObject created = createUpload(kChunkSize + kTailBytes, kChunkSize,
                                             kChunkCount, m_sha256);
    QCOMPARE(codeOf(created), int(ErrorCode::Ok));
    const qint64 aliceFileId = jsonToId(created.value("data").toObject().value("fileId"));

    // 同一连接上换人：register 免鉴权，login 会改写该连接的会话状态
    QCOMPARE(codeOf(request(MessageType::RegisterRequest, "register",
                            { { "username", "cp_bob" }, { "password", "test123456" } })),
             int(ErrorCode::Ok));
    const QJsonObject login = request(MessageType::LoginRequest, "login",
                                      { { "username", "cp_bob" },
                                        { "password", "test123456" },
                                        { "deviceId", "cp-device-b" },
                                        { "clientVersion", "test" },
                                        { "platform", "windows" } });
    QCOMPARE(codeOf(login), int(ErrorCode::Ok));
    m_token = login.value("data").toObject().value("token").toString();
    QVERIFY(!m_token.isEmpty());

    const int notYours = codeOf(request(MessageType::FileUploadQueryRequest,
                                        "file_upload_query", { { "fileId", aliceFileId } }));
    const int notExist = codeOf(request(MessageType::FileUploadQueryRequest,
                                        "file_upload_query", { { "fileId", 999999 } }));
    QCOMPARE(notYours, int(ErrorCode::FileNotFound));
    QCOMPARE(notYours, notExist);

    const int ticketNotYours = codeOf(request(MessageType::FileDownloadTicketRequest,
                                              "file_download_ticket",
                                              { { "fileId", aliceFileId } }));
    const int ticketNotExist = codeOf(request(MessageType::FileDownloadTicketRequest,
                                              "file_download_ticket", { { "fileId", 999999 } }));
    QCOMPARE(ticketNotYours, int(ErrorCode::FileNotFound));
    QCOMPARE(ticketNotYours, ticketNotExist);
}

// 分片不齐回可恢复的 3016 并附已收索引，客户端据此只补传缺的那几片
void TestFileControlPlane::completeReportsIncompleteWithReceivedIndexes()
{
    const QJsonObject created = createUpload(kChunkSize + kTailBytes, kChunkSize,
                                             kChunkCount, m_sha256);
    QCOMPARE(codeOf(created), int(ErrorCode::Ok));
    const qint64 fileId = jsonToId(created.value("data").toObject().value("fileId"));

    const QJsonObject none = request(MessageType::FileUploadCompleteRequest,
                                     "file_upload_complete", { { "fileId", fileId } });
    QCOMPARE(codeOf(none), int(ErrorCode::FileUploadIncomplete));
    const QJsonObject noneData = none.value("data").toObject();
    QCOMPARE(noneData.value("status").toString(), QString(FileStatus::Uploading));
    QCOMPARE(noneData.value("receivedChunks").toArray().size(), 0);

    const QString blobKey = blobKeyFor(fileId);
    QVERIFY(!blobKey.isEmpty());
    QVERIFY(m_storage->putChunk(blobKey, 0, m_chunk0));

    const QJsonObject partial = request(MessageType::FileUploadCompleteRequest,
                                        "file_upload_complete", { { "fileId", fileId } });
    QCOMPARE(codeOf(partial), int(ErrorCode::FileUploadIncomplete));
    const QJsonArray received = partial.value("data").toObject().value("receivedChunks").toArray();
    QCOMPARE(received.size(), 1);
    QCOMPARE(received.at(0).toInt(), 0);
}

// 数据故障（分片长度/整体校验和不符）回 3015 并回收，不保留"可重传"的假希望：
// 重传同一批分片只会得到同样结果
void TestFileControlPlane::completeRejectsDataFaultAndMarksFailed()
{
    const QJsonObject created = createUpload(kChunkSize + kTailBytes, kChunkSize,
                                             kChunkCount, m_sha256);
    QCOMPARE(codeOf(created), int(ErrorCode::Ok));
    const qint64 fileId = jsonToId(created.value("data").toObject().value("fileId"));

    const QString blobKey = blobKeyFor(fileId);
    QVERIFY(!blobKey.isEmpty());
    QVERIFY(m_storage->putChunk(blobKey, 0, m_chunk0));
    // 末片长度与 expectedChunkBytes 不符（模拟截断上传）
    QVERIFY(m_storage->putChunk(blobKey, 1, m_chunk1.left(10)));

    QCOMPARE(codeOf(request(MessageType::FileUploadCompleteRequest, "file_upload_complete",
                            { { "fileId", fileId } })),
             int(ErrorCode::FileChecksumMismatch));

    // 已标 failed：既不能再完成，也不能下载；磁盘现场已回收
    QCOMPARE(codeOf(request(MessageType::FileUploadCompleteRequest, "file_upload_complete",
                            { { "fileId", fileId } })),
             int(ErrorCode::FileNotReady));
    QCOMPARE(codeOf(request(MessageType::FileDownloadTicketRequest, "file_download_ticket",
                            { { "fileId", fileId } })),
             int(ErrorCode::FileNotReady));
    QVERIFY(m_storage->receivedChunks(blobKey).isEmpty());
}

// 完成后重试必须成功（响应丢失后客户端会重试），且此时才能签下载票据
void TestFileControlPlane::completeIsIdempotentAndEnablesDownloadTicket()
{
    const QJsonObject created = createUpload(kChunkSize + kTailBytes, kChunkSize,
                                             kChunkCount, m_sha256);
    QCOMPARE(codeOf(created), int(ErrorCode::Ok));
    const qint64 fileId = jsonToId(created.value("data").toObject().value("fileId"));

    const QString blobKey = blobKeyFor(fileId);
    QVERIFY(!blobKey.isEmpty());
    QVERIFY(m_storage->putChunk(blobKey, 0, m_chunk0));
    QVERIFY(m_storage->putChunk(blobKey, 1, m_chunk1));

    const QJsonObject done = request(MessageType::FileUploadCompleteRequest,
                                     "file_upload_complete", { { "fileId", fileId } });
    QCOMPARE(codeOf(done), int(ErrorCode::Ok));
    const QJsonObject doneData = done.value("data").toObject();
    QCOMPARE(doneData.value("status").toString(), QString(FileStatus::Ready));
    QCOMPARE(doneData.value("sha256").toString(), m_sha256);
    QVERIFY(m_storage->isFinalized(blobKey));

    QCOMPARE(codeOf(request(MessageType::FileUploadCompleteRequest, "file_upload_complete",
                            { { "fileId", fileId } })),
             int(ErrorCode::Ok));

    const QJsonObject ticket = request(MessageType::FileDownloadTicketRequest,
                                       "file_download_ticket", { { "fileId", fileId } });
    QCOMPARE(codeOf(ticket), int(ErrorCode::Ok));
    const QJsonObject ticketData = ticket.value("data").toObject();
    QVERIFY(!ticketData.value("downloadTicket").toString().isEmpty());
    QCOMPARE(jsonToId(ticketData.value("sizeBytes")), kChunkSize + kTailBytes);
    QCOMPARE(jsonToId(ticketData.value("chunkSize")), kChunkSize);
    QCOMPARE(ticketData.value("chunkCount").toInt(), kChunkCount);
    QCOMPARE(ticketData.value("sha256").toString(), m_sha256);
    QCOMPARE(ticketData.value("expiresInSeconds").toInt(), DownloadTicketTtlSeconds);
    QVERIFY(!ticketData.contains("blobKey"));

    // 终态查询不再数盘：status 为 ready、receivedChunks 为空
    const QJsonObject query = request(MessageType::FileUploadQueryRequest, "file_upload_query",
                                      { { "fileId", fileId } });
    QCOMPARE(codeOf(query), int(ErrorCode::Ok));
    QCOMPARE(query.value("data").toObject().value("status").toString(), QString(FileStatus::Ready));
    QCOMPARE(query.value("data").toObject().value("receivedChunks").toArray().size(), 0);
}

// 取消幂等，且取消后不能再完成；已取消的上传不保留磁盘分片
void TestFileControlPlane::cancelIsIdempotentAndBlocksComplete()
{
    const QJsonObject created = createUpload(kChunkSize + kTailBytes, kChunkSize,
                                             kChunkCount, m_sha256);
    QCOMPARE(codeOf(created), int(ErrorCode::Ok));
    const qint64 fileId = jsonToId(created.value("data").toObject().value("fileId"));

    const QString blobKey = blobKeyFor(fileId);
    QVERIFY(!blobKey.isEmpty());
    QVERIFY(m_storage->putChunk(blobKey, 0, m_chunk0));

    const QJsonObject cancelled = request(MessageType::FileUploadCancelRequest,
                                          "file_upload_cancel", { { "fileId", fileId } });
    QCOMPARE(codeOf(cancelled), int(ErrorCode::Ok));
    QCOMPARE(cancelled.value("data").toObject().value("status").toString(), QString(FileStatus::Cancelled));
    QVERIFY(m_storage->receivedChunks(blobKey).isEmpty());

    QCOMPARE(codeOf(request(MessageType::FileUploadCancelRequest, "file_upload_cancel",
                            { { "fileId", fileId } })),
             int(ErrorCode::Ok));
    QCOMPARE(codeOf(request(MessageType::FileUploadCompleteRequest, "file_upload_complete",
                            { { "fileId", fileId } })),
             int(ErrorCode::FileNotReady));
}

QTEST_GUILESS_MAIN(TestFileControlPlane)
#include "TestFileControlPlane.moc"
