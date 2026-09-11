// M8.2: 文件传输数据面（HTTP(S)）集成测试
//
// 覆盖无法靠编译保证的行为：路由匹配、票据授权、分片长度与序号校验、
// Range 语义、状态护栏、失败限流，以及"上传 -> 组装 -> 下载"的字节级往返。
//
// 以明文 HTTP + 端口 0（OS 分配）运行：TLS 路径与主通道共用同一份
// QSslConfiguration，其 fail-closed 策略由 startIsFailClosed 用例单独断言，
// 无需真实证书即可覆盖。
#include <QtTest>
#include <QEventLoop>
#include <QHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslConfiguration>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUrl>

#include <memory>

#include "database/DatabaseManager.h"
#include "http/FileHttpService.h"
#include "storage/LocalFileStorage.h"

#include "FileCrypto.h"
#include "FileProtocol.h"

using namespace XYChat::Server;
using XYChat::Security::FileCrypto;
namespace Protocol = XYChat::Protocol;

namespace
{
const char TicketHeader[] = "X-XYChat-Ticket";
}

class TestFileHttpService : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void startIsFailClosed();
    void uploadThenDownloadRoundTripsBytes();
    void ticketAuthorizationIsFailClosed();
    void chunkPutRejectsMalformedRequests();
    void stateGuardsAreEnforced();
    void downloadHonoursRangeAndLimits();
    void chunkPutOverwritesIdempotently();
    void revokedTicketStopsWorking();
    // 必须最后执行：本用例会令本机 IP 进入失败限流窗口，之后的请求都会 429
    void repeatedAuthFailuresAreRateLimited();

private:
    struct HttpReply
    {
        int status = 0;
        QByteArray body;
        QHash<QByteArray, QByteArray> headers;  // 键已统一小写

        // HTTP 头名大小写不敏感：存储时已 toLower，查询也必须 toLower
        QByteArray header(const char *name) const
        {
            return headers.value(QByteArray(name).toLower());
        }
    };

    // 一份可用的文件夹具：记录 + 存储键 + 期望字节 + 票据
    struct Fixture
    {
        qint64 fileId = 0;
        QString blobKey;
        QByteArray blob;
        qint64 chunkSize = 0;
        int chunkCount = 0;
        QString uploadTicket;
        QString downloadTicket;
    };

    HttpReply send(QNetworkAccessManager &nam, QNetworkRequest request, const QByteArray *body);
    HttpReply put(const QString &path, const QByteArray &body, const QString &ticket);
    HttpReply get(const QString &path, const QString &ticket,
                  const QByteArray &range = QByteArray());

    // 造一条 uploading 记录并签发上传票据（不写任何分片）。
    // lastChunkBytes > 0 时末片为该长度（余量），否则末片与其余分片等长
    void makeUploadingFile(int chunkCount, qint64 chunkSize, Fixture *out,
                           qint64 lastChunkBytes = -1);
    // 经 HTTP PUT 上传全部分片，再走存储层组装与元数据 markFileReady
    //（模拟控制面 file_upload_complete 的效果），最后签发下载票据
    void makeReadyFile(int chunkCount, qint64 chunkSize, Fixture *out,
                       qint64 lastChunkBytes = -1);
    QString issueTicket(qint64 fileId, const QString &kind, int ttlSeconds);

    QTemporaryDir *m_dir = nullptr;
    std::unique_ptr<LocalFileStorage> m_storage;
    std::unique_ptr<FileHttpService> m_service;
    DatabaseManager *m_db = nullptr;
    QString m_connName;
    QString m_base;
    qint64 m_userId = 0;
};

void TestFileHttpService::initTestCase()
{
    m_dir = new QTemporaryDir();
    QVERIFY(m_dir->isValid());
    m_storage = std::make_unique<LocalFileStorage>(m_dir->path());
    QVERIFY(m_storage->initialize());

    // 服务与测试共享同一份 schema：先按服务的连接名建内存库，双方都用它
    //（DatabaseManager 析构会 removeDatabase，故连接名必须唯一且共用）
    m_connName = "test_file_http";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", m_connName);
        db.setDatabaseName(":memory:");
        QVERIFY(db.open());
    }
    m_db = new DatabaseManager(m_connName);
    QVERIFY(m_db->initialize());

    m_userId = m_db->registerUser("httpuser", QString(), QString(), "v1:1:salt:hash");
    QVERIFY(m_userId > 0);

    m_service = std::make_unique<FileHttpService>(nullptr, m_connName);
    m_service->setObjectStorage(m_storage.get());
    // 端口 0 交由 OS 分配，baseUrl() 取实际监听端口
    QVERIFY(m_service->start(0, QSslConfiguration(), false, true));
    QVERIFY(m_service->isListening());
    QVERIFY(m_service->serverPort() > 0);

    m_base = m_service->baseUrl();
    QVERIFY(m_base.startsWith("http://127.0.0.1:"));
    QVERIFY(m_base.endsWith("/file"));
}

void TestFileHttpService::cleanupTestCase()
{
    // 先销毁服务（其 DatabaseManager 析构会移除连接），再清理其余资源
    m_service.reset();
    delete m_db;
    m_db = nullptr;
    {
        QSqlDatabase db = QSqlDatabase::database(m_connName, false);
        if (db.isOpen()) {
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(m_connName);
    m_storage.reset();
    delete m_dir;
    m_dir = nullptr;
}

void TestFileHttpService::startIsFailClosed()
{
    // 无对象存储：数据面拒绝启动（起来后对每个请求回 500 毫无意义）
    {
        const QString conn = "test_file_http_no_storage";
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", conn);
            db.setDatabaseName(":memory:");
            QVERIFY(db.open());
        }
        FileHttpService svc(nullptr, conn);
        QVERIFY(!svc.isListening());
        QVERIFY(!svc.start(0, QSslConfiguration(), false, true));
        QVERIFY(svc.baseUrl().isEmpty());
        QCOMPARE(svc.serverPort(), quint16(0));
    }

    // TLS 不可用且未显式允许明文：拒绝启动（与主通道同一 fail-closed 口径）
    {
        const QString conn = "test_file_http_no_tls";
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", conn);
            db.setDatabaseName(":memory:");
            QVERIFY(db.open());
        }
        FileHttpService svc(nullptr, conn);
        svc.setObjectStorage(m_storage.get());
        QVERIFY(!svc.start(0, QSslConfiguration(), false, false));
        QVERIFY(!svc.isListening());
    }

    // 通告主机名可覆盖（NAT/反向代理场景），并反映到 baseUrl
    {
        const QString conn = "test_file_http_host";
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", conn);
            db.setDatabaseName(":memory:");
            QVERIFY(db.open());
        }
        FileHttpService svc(nullptr, conn);
        svc.setObjectStorage(m_storage.get());
        svc.setAdvertisedHost("files.example.com");
        QVERIFY(svc.start(0, QSslConfiguration(), false, true));
        QVERIFY(svc.baseUrl().startsWith("http://files.example.com:"));
        QVERIFY(svc.baseUrl().endsWith("/file"));
        // 空主机名不得覆盖已有值（否则 baseUrl 会拼出 "://"）
        svc.setAdvertisedHost(QString());
        QVERIFY(svc.baseUrl().contains("files.example.com"));
    }
}

void TestFileHttpService::uploadThenDownloadRoundTripsBytes()
{
    // 2 片 x 1 MiB = 2 MiB，未超单次 GET 上限，可全量取
    Fixture f;
    makeReadyFile(2, 1024 * 1024, &f);
    QVERIFY(f.fileId > 0);
    QCOMPARE(f.blob.size(), qsizetype(2 * 1024 * 1024));

    const HttpReply reply = get(m_base + "/" + QString::number(f.fileId), f.downloadTicket);
    QCOMPARE(reply.status, 200);
    // 字节级一致：数据面不得改写、补齐或截断密文
    QCOMPARE(reply.body, f.blob);

    // 下载恒按二进制投递：服务端不知道真实 MIME，也不得猜测或回填
    QCOMPARE(reply.header("Content-Type"), QByteArray("application/octet-stream"));
    QCOMPARE(reply.header("Accept-Ranges"), QByteArray("bytes"));
    // 密文也不得被中间缓存：票据在请求头里，缓存命中会绕过授权
    QCOMPARE(reply.header("Cache-Control"), QByteArray("no-store"));
    QCOMPARE(reply.header("X-XYChat-Total-Size"), QByteArray::number(f.blob.size()));
    // 存储键不出服务端（它若泄露就成了可枚举的对象路径）
    QVERIFY(!reply.body.contains(f.blobKey.toLatin1()));
    for (auto it = reply.headers.constBegin(); it != reply.headers.constEnd(); ++it) {
        QVERIFY(!it.value().contains(f.blobKey.toLatin1()));
    }
}

void TestFileHttpService::ticketAuthorizationIsFailClosed()
{
    Fixture up;
    makeUploadingFile(2, 64 * 1024, &up);
    QVERIFY(up.fileId > 0);
    Fixture ready;
    makeReadyFile(1, 64 * 1024, &ready);
    QVERIFY(ready.fileId > 0);

    const QByteArray chunk(64 * 1024, 'x');
    const QString putPath = m_base + "/" + QString::number(up.fileId) + "/chunk/0";
    const QString getPath = m_base + "/" + QString::number(ready.fileId);

    // 四种"票据本身不可用"的失败一律 401 且响应体完全一致：差异化响应会让
    // 本端点变成"某票据是否存在/属于哪个文件/是何用途"的探测预言机。
    // （"未携带票据"单独一种文案：那是请求格式问题，不泄露任何票据库信息，
    // 且区分它能让客户端发现自己忘了带头）
    const HttpReply missing = put(putPath, chunk, QString());
    QCOMPARE(missing.status, 401);

    const HttpReply garbage = put(putPath, chunk, QString(64, 'z'));
    QCOMPARE(garbage.status, 401);

    // 用途不符：下载票据不能用于上传
    const HttpReply wrongKind = put(putPath, chunk, ready.downloadTicket);
    QCOMPARE(wrongKind.status, 401);
    QCOMPARE(wrongKind.body, garbage.body);

    // 绑定不符：票据属于另一个文件
    const HttpReply wrongFile = put(getPath + "/chunk/0", chunk, up.uploadTicket);
    QCOMPARE(wrongFile.status, 401);
    QCOMPARE(wrongFile.body, garbage.body);

    // 反向：上传票据不能用于下载
    const HttpReply getWrongKind = get(getPath, up.uploadTicket);
    QCOMPARE(getWrongKind.status, 401);
    QCOMPARE(getWrongKind.body, garbage.body);

    // 过期票据（TTL 必须为正，故用 SQL 把 expires_at 拨到过去）
    const QString expired = issueTicket(ready.fileId, Protocol::FileTicketKind::Download, 300);
    QVERIFY(!expired.isEmpty());
    QSqlQuery q(QSqlDatabase::database(m_connName));
    q.prepare("UPDATE file_tickets SET expires_at = datetime('now', '-2 seconds') "
              "WHERE ticket_hash = ?");
    q.addBindValue(FileCrypto::ticketHash(expired));
    QVERIFY(q.exec());
    const HttpReply expiredReply = get(getPath, expired);
    QCOMPARE(expiredReply.status, 401);
    // 过期与不存在/类型不符/绑定不符同文案，不向调用方泄露具体原因
    QCOMPARE(expiredReply.body, garbage.body);

    // 上述失败均不得写入任何分片（授权先于业务）
    QVERIFY(m_storage->receivedChunks(up.blobKey).isEmpty());
}

void TestFileHttpService::chunkPutRejectsMalformedRequests()
{
    // 2 片：首片 64 KiB、末片 32 KiB（余量）
    Fixture f;
    makeUploadingFile(2, 64 * 1024, &f, 32 * 1024);
    QVERIFY(f.fileId > 0);
    const QByteArray good(64 * 1024, 'a');
    const QString base = m_base + "/" + QString::number(f.fileId) + "/chunk/";

    // 长度必须精确匹配该分片的期望值：客户端自选分片边界会绕过体积与分片数校验
    const HttpReply shortPut = put(base + "0", good.left(1024), f.uploadTicket);
    QCOMPARE(shortPut.status, 400);
    QVERIFY(shortPut.body.contains("expectedBytes"));
    QVERIFY(shortPut.body.contains(QByteArray::number(64 * 1024)));

    QCOMPARE(put(base + "0", good + QByteArray(16, 'b'), f.uploadTicket).status, 400);
    // 序号越界（分片数之外）与荒谬值
    QCOMPARE(put(base + "2", good, f.uploadTicket).status, 400);
    QCOMPARE(put(base + "999999", good, f.uploadTicket).status, 400);

    // 畸形路径参数：非数字、负号、零 fileId、尾随字符一律 400（不是 500）
    QCOMPARE(put(m_base + "/abc/chunk/0", good, f.uploadTicket).status, 400);
    QCOMPARE(put(m_base + "/-1/chunk/0", good, f.uploadTicket).status, 400);
    QCOMPARE(put(m_base + "/0/chunk/0", good, f.uploadTicket).status, 400);
    QCOMPARE(put(base + "-1", good, f.uploadTicket).status, 400);
    QCOMPARE(put(base + "1x", good, f.uploadTicket).status, 400);

    // 上述全部被拒的请求都不得留下分片
    QVERIFY(m_storage->receivedChunks(f.blobKey).isEmpty());

    // 末片为余量（32 KiB）：用满片长度冒充末片必须被拒，否则服务端
    // 组装出的对象与声明的体积/校验和不符，客户端只能全量重传
    QCOMPARE(put(base + "1", good, f.uploadTicket).status, 400);
    QCOMPARE(put(base + "1", good.left(32 * 1024), f.uploadTicket).status, 200);
    QCOMPARE(put(base + "0", good, f.uploadTicket).status, 200);
    QCOMPARE(m_storage->receivedChunks(f.blobKey).size(), 2);

    // 超大分片：413，不把数据交给存储层
    Fixture big;
    makeUploadingFile(1, Protocol::MaxChunkSize, &big);
    QVERIFY(big.fileId > 0);
    const QByteArray huge(Protocol::MaxChunkSize + 1, 'z');
    const HttpReply tooBig = put(m_base + "/" + QString::number(big.fileId) + "/chunk/0",
                                 huge, big.uploadTicket);
    QCOMPARE(tooBig.status, 413);
    QVERIFY(m_storage->receivedChunks(big.blobKey).isEmpty());
}

void TestFileHttpService::stateGuardsAreEnforced()
{
    Fixture up;
    makeUploadingFile(1, 64 * 1024, &up);
    QVERIFY(up.fileId > 0);
    Fixture ready;
    makeReadyFile(1, 64 * 1024, &ready);
    QVERIFY(ready.fileId > 0);

    // 未完成的文件不可下载（票据有效也不放行：状态只对持票者可见，不构成全局预言机）
    const QString upDownloadTicket = issueTicket(up.fileId, Protocol::FileTicketKind::Download, 300);
    QVERIFY(!upDownloadTicket.isEmpty());
    const HttpReply getUploading = get(m_base + "/" + QString::number(up.fileId), upDownloadTicket);
    QCOMPARE(getUploading.status, 409);

    // 已完成的文件不再接受分片（组装后分片已回收，继续写会产出无主数据）
    const QByteArray chunk(64 * 1024, 'c');
    const HttpReply putReady = put(m_base + "/" + QString::number(ready.fileId) + "/chunk/0",
                                   chunk, ready.uploadTicket);
    QCOMPARE(putReady.status, 409);

    // 不存在的 fileId：票据与文件双向绑定，因此仍是 401 而不是 404
    const HttpReply ghost = get(m_base + "/99999999", ready.downloadTicket);
    QCOMPARE(ghost.status, 401);
}

void TestFileHttpService::downloadHonoursRangeAndLimits()
{
    // 5 MiB（2 片：4 MiB + 1 MiB 余量）超过单次 GET 上限，必须分段取
    Fixture big;
    makeReadyFile(2, Protocol::MaxChunkSize, &big, 1024 * 1024);
    QVERIFY(big.fileId > 0);
    QCOMPARE(big.blob.size(), qsizetype(5) * 1024 * 1024);
    const QString path = m_base + "/" + QString::number(big.fileId);
    const qint64 firstChunk = Protocol::MaxChunkSize;

    // 无 Range 且超上限：413 + Accept-Ranges 告知客户端改用分段
    const HttpReply whole = get(path, big.downloadTicket);
    QCOMPARE(whole.status, 413);
    QCOMPARE(whole.header("Accept-Ranges"), QByteArray("bytes"));
    QCOMPARE(whole.header("X-XYChat-Total-Size"), QByteArray::number(big.blob.size()));
    QVERIFY(whole.body.isEmpty());

    // 首片（与分片边界对齐）
    const HttpReply first = get(path, big.downloadTicket,
                                "bytes=0-" + QByteArray::number(firstChunk - 1));
    QCOMPARE(first.status, 206);
    QCOMPARE(first.body, big.blob.left(firstChunk));
    QCOMPARE(first.header("Content-Range"),
             QByteArray("bytes 0-") + QByteArray::number(firstChunk - 1) + "/"
                 + QByteArray::number(big.blob.size()));

    // 末片（开区间上界，终点由服务端补全）
    const HttpReply last = get(path, big.downloadTicket,
                               "bytes=" + QByteArray::number(firstChunk) + "-");
    QCOMPARE(last.status, 206);
    QCOMPARE(last.body, big.blob.mid(firstChunk));
    QCOMPARE(last.body.size(), qsizetype(1024 * 1024));

    // 终点超出对象长度：按 RFC 截断到末尾而不是报错
    const HttpReply clamped = get(path, big.downloadTicket,
                                 "bytes=" + QByteArray::number(firstChunk) + "-99999999");
    QCOMPARE(clamped.status, 206);
    QCOMPARE(clamped.body, big.blob.mid(firstChunk));

    // 请求区间长度超上限：413（防止用 Range 绕过分段限制把整个文件读进内存）
    QCOMPARE(get(path, big.downloadTicket, "bytes=0-99999999").status, 413);

    // 起点越界：416 + Content-Range 告知总长（与 400 区分，客户端据此重算偏移）
    const HttpReply unsatisfiable = get(path, big.downloadTicket, "bytes=99999999-");
    QCOMPARE(unsatisfiable.status, 416);
    QCOMPARE(unsatisfiable.header("Content-Range"),
             QByteArray("bytes */") + QByteArray::number(big.blob.size()));

    // range-unit 大小写不敏感（RFC 9110）：归为语法错误会让合法请求
    // 白吃一次 per-IP 失败配额
    QCOMPARE(get(path, big.downloadTicket, "Bytes=0-1023").status, 206);

    // 语法非法：多区间、后缀形式、非数字、倒置区间、错误单位一律 400
    QCOMPARE(get(path, big.downloadTicket, "bytes=0-1,2-3").status, 400);
    QCOMPARE(get(path, big.downloadTicket, "bytes=-100").status, 400);
    QCOMPARE(get(path, big.downloadTicket, "bytes=abc-def").status, 400);
    QCOMPARE(get(path, big.downloadTicket, "bytes=100-50").status, 400);
    QCOMPARE(get(path, big.downloadTicket, "items=0-10").status, 400);

    // 未超上限的小文件仍可全量取（无 Range 不必强制分段）
    Fixture small;
    makeReadyFile(2, 64 * 1024, &small);
    QVERIFY(small.fileId > 0);
    const HttpReply smallWhole = get(m_base + "/" + QString::number(small.fileId),
                                     small.downloadTicket);
    QCOMPARE(smallWhole.status, 200);
    QCOMPARE(smallWhole.body, small.blob);
}

void TestFileHttpService::chunkPutOverwritesIdempotently()
{
    Fixture f;
    makeUploadingFile(1, 64 * 1024, &f);
    QVERIFY(f.fileId > 0);
    const QString path = m_base + "/" + QString::number(f.fileId) + "/chunk/0";

    const QByteArray first(64 * 1024, 'a');
    QCOMPARE(put(path, first, f.uploadTicket).status, 200);
    QCOMPARE(m_storage->readChunk(f.blobKey, 0), first);

    // 重传同一片为幂等覆盖：客户端失败重试不应被拒，也不应产生重复数据
    const QByteArray second(64 * 1024, 'b');
    QCOMPARE(put(path, second, f.uploadTicket).status, 200);
    QCOMPARE(m_storage->readChunk(f.blobKey, 0), second);
    QCOMPARE(m_storage->receivedChunks(f.blobKey).size(), 1);
}

void TestFileHttpService::revokedTicketStopsWorking()
{
    Fixture f;
    makeUploadingFile(1, 64 * 1024, &f);
    QVERIFY(f.fileId > 0);
    const QByteArray chunk(64 * 1024, 'r');
    const QString path = m_base + "/" + QString::number(f.fileId) + "/chunk/0";

    QCOMPARE(put(path, chunk, f.uploadTicket).status, 200);

    // 控制面在上传完成/取消/失败后吊销上传票据，此后同一票据必须立即失效
    QVERIFY(m_db->revokeFileTickets(f.fileId, Protocol::FileTicketKind::Upload) >= 1);
    QCOMPARE(put(path, chunk, f.uploadTicket).status, 401);

    // 入参兜底与幂等（已无票据时返回 0 而不是错误，回收任务可无条件调用）
    QCOMPARE(m_db->revokeFileTickets(0, Protocol::FileTicketKind::Upload), -1);
    QCOMPARE(m_db->revokeFileTickets(f.fileId, QString()), -1);
    QCOMPARE(m_db->revokeFileTickets(f.fileId, Protocol::FileTicketKind::Upload), 0);
}

void TestFileHttpService::repeatedAuthFailuresAreRateLimited()
{
    // 不假设起始计数为 0：前面的用例已为本机 IP 累计过若干次失败
    int limitedAt = -1;
    for (int i = 0; i < 120; ++i) {
        const HttpReply reply = get(m_base + "/1", QString());
        if (reply.status == 429) {
            limitedAt = i;
            break;
        }
        QCOMPARE(reply.status, 401);
    }
    QVERIFY2(limitedAt >= 0, "failure window never triggered 429");
    // 窗口内持续拒绝（票据爆破与未授权扫描在此被挡住）
    QCOMPARE(get(m_base + "/1", QString()).status, 429);
    QCOMPARE(put(m_base + "/1/chunk/0", QByteArray("x"), QString()).status, 429);
}

TestFileHttpService::HttpReply TestFileHttpService::send(QNetworkAccessManager &nam,
                                                         QNetworkRequest request,
                                                         const QByteArray *body)
{
    QNetworkReply *reply = body ? nam.put(request, *body) : nam.get(request);
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    HttpReply result;
    result.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    result.body = reply->readAll();
    const QList<QByteArray> names = reply->rawHeaderList();
    for (const QByteArray &name : names) {
        result.headers.insert(name.toLower(), reply->rawHeader(name));
    }
    reply->deleteLater();
    return result;
}

TestFileHttpService::HttpReply TestFileHttpService::put(const QString &path,
                                                        const QByteArray &body,
                                                        const QString &ticket)
{
    QNetworkAccessManager nam;
    QNetworkRequest request{QUrl(path)};
    if (!ticket.isEmpty()) {
        request.setRawHeader(TicketHeader, ticket.toLatin1());
    }
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
    return send(nam, request, &body);
}

TestFileHttpService::HttpReply TestFileHttpService::get(const QString &path,
                                                        const QString &ticket,
                                                        const QByteArray &range)
{
    QNetworkAccessManager nam;
    QNetworkRequest request{QUrl(path)};
    if (!ticket.isEmpty()) {
        request.setRawHeader(TicketHeader, ticket.toLatin1());
    }
    if (!range.isEmpty()) {
        request.setRawHeader("Range", range);
    }
    return send(nam, request, nullptr);
}

QString TestFileHttpService::issueTicket(qint64 fileId, const QString &kind, int ttlSeconds)
{
    const QString ticket = FileCrypto::generateTicket();
    if (ticket.isEmpty()) {
        return QString();
    }
    if (!m_db->issueFileTicket(fileId, m_userId, kind, FileCrypto::ticketHash(ticket),
                               ttlSeconds)) {
        return QString();
    }
    return ticket;
}

void TestFileHttpService::makeUploadingFile(int chunkCount, qint64 chunkSize, Fixture *out,
                                            qint64 lastChunkBytes)
{
    QVERIFY(out != nullptr);
    QVERIFY(chunkCount > 0 && chunkSize > 0);
    QVERIFY(lastChunkBytes <= 0 || lastChunkBytes <= chunkSize);
    out->chunkSize = chunkSize;
    out->chunkCount = chunkCount;

    // 末片为余量时体积不是 chunkSize 的整数倍，这才能验证数据面对末片
    // 长度的校验（用满片长度冒充末片必须被拒）
    const qint64 tail = lastChunkBytes > 0 ? lastChunkBytes : chunkSize;
    const qint64 sizeBytes = chunkSize * (chunkCount - 1) + tail;
    // 每片填充不同字节，使拼接顺序错误在比对时立刻暴露
    for (int i = 0; i < chunkCount; ++i) {
        const qint64 n = (i == chunkCount - 1) ? tail : chunkSize;
        out->blob += QByteArray(n, static_cast<char>('A' + i));
    }
    QCOMPARE(out->blob.size(), sizeBytes);

    out->blobKey = m_storage->allocateBlobKey();
    QVERIFY(!out->blobKey.isEmpty());
    out->fileId = m_db->createFileRecord(m_userId, "deviceA", out->blobKey, sizeBytes, chunkSize,
                                         chunkCount, FileCrypto::sha256Hex(out->blob));
    QVERIFY(out->fileId > 0);
    out->uploadTicket = issueTicket(out->fileId, Protocol::FileTicketKind::Upload, 3600);
    QVERIFY(!out->uploadTicket.isEmpty());
}

void TestFileHttpService::makeReadyFile(int chunkCount, qint64 chunkSize, Fixture *out,
                                        qint64 lastChunkBytes)
{
    makeUploadingFile(chunkCount, chunkSize, out, lastChunkBytes);
    QVERIFY(out->fileId > 0);

    // 经真实 HTTP 路径逐片上传，顺带验证 PUT 端点本身（mid 对末片不足长
    // 的情况自动截到末尾，与 expectedChunkBytes 的口径一致）
    for (int i = 0; i < chunkCount; ++i) {
        const QByteArray chunk = out->blob.mid(chunkSize * i, chunkSize);
        const HttpReply reply = put(m_base + "/" + QString::number(out->fileId) + "/chunk/"
                                        + QString::number(i),
                                    chunk, out->uploadTicket);
        QCOMPARE(reply.status, 200);
    }
    QCOMPARE(m_storage->receivedChunks(out->blobKey).size(), chunkCount);

    // 模拟控制面 file_upload_complete：组装 + 整体校验 + 转 ready
    QCOMPARE(m_storage->finalize(out->blobKey, out->blob.size(), chunkSize, chunkCount,
                                 FileCrypto::sha256Hex(out->blob)),
             IObjectStorage::FinalizeStatus::Ok);
    QVERIFY(m_db->markFileReady(out->fileId));

    out->downloadTicket = issueTicket(out->fileId, Protocol::FileTicketKind::Download, 300);
    QVERIFY(!out->downloadTicket.isEmpty());
}

QTEST_GUILESS_MAIN(TestFileHttpService)
#include "TestFileHttpService.moc"
