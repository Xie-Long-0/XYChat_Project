#include "FileHttpService.h"

#include <QDateTime>
#include <QHostAddress>
#include <QHttpHeaders>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSslConfiguration>
#include <QSslServer>
#include <QTcpServer>

#include "FileCrypto.h"
#include "FileProtocol.h"
#include "StructuredLogger.h"

namespace XYChat::Server
{

namespace
{

using XYChat::Security::FileCrypto;
using XYChat::Security::StructuredLogger;
namespace Protocol = XYChat::Protocol;

using Status = QHttpServerResponder::StatusCode;

// 票据只经请求头传递（见头文件说明），header 名大小写不敏感
const char TicketHeader[] = "X-XYChat-Ticket";
// 下载恒按二进制投递：服务端不知道真实 MIME，也不得猜测
const char DownloadMimeType[] = "application/octet-stream";

void respondError(QHttpServerResponder &responder, Status status, const QString &error)
{
    QJsonObject obj;
    obj["error"] = error;
    responder.write(QJsonDocument(obj), status);
}

// Range 解析结果：语法非法与"语法合法但不可满足"必须分开，前者回 400、
// 后者按 RFC 9110 回 416（混淆会让客户端把服务端问题当成自己的请求格式问题）
enum class RangeResult
{
    Absent,         // 未携带 Range 头
    Ok,             // 单区间且可满足
    Malformed,      // 语法非法（多区间、后缀形式、非数字等）
    Unsatisfiable,  // 起点超出对象长度
};

// 只支持单区间 bytes=<start>-[<end>]。刻意不接受后缀形式（bytes=-500）与多区间：
// 分片是独立 AEAD 加密的，客户端只会按分片边界取，支持更多形式只增加解析面
RangeResult parseRange(const QByteArray &header, qint64 blobSize, qint64 *offset, qint64 *length)
{
    *offset = 0;
    *length = 0;
    if (header.isEmpty()) {
        return RangeResult::Absent;
    }
    // range-unit 大小写不敏感（RFC 9110）：把 "Bytes=0-1" 归为语法错误会让
    // 合法请求（第三方客户端/代理改写头大小写）白吃一次 per-IP 失败配额
    if (header.left(6).compare("bytes=", Qt::CaseInsensitive) != 0) {
        return RangeResult::Malformed;
    }
    const QByteArray spec = header.mid(6).trimmed();
    if (spec.isEmpty() || spec.contains(',') || spec.startsWith('-')) {
        return RangeResult::Malformed;
    }
    const int dash = spec.indexOf('-');
    if (dash <= 0) {
        return RangeResult::Malformed;
    }
    bool startOk = false;
    bool endOk = false;
    const qint64 start = spec.left(dash).trimmed().toLongLong(&startOk);
    if (!startOk || start < 0) {
        return RangeResult::Malformed;
    }
    const QByteArray endText = spec.mid(dash + 1).trimmed();
    qint64 end = blobSize - 1;
    if (!endText.isEmpty()) {
        end = endText.toLongLong(&endOk);
        if (!endOk || end < 0) {
            return RangeResult::Malformed;
        }
    }
    if (start >= blobSize) {
        return RangeResult::Unsatisfiable;
    }
    // 终点超界按 RFC 截断到末尾（客户端不必知道精确长度也能安全请求"到结尾"）
    if (end > blobSize - 1) {
        end = blobSize - 1;
    }
    if (end < start) {
        return RangeResult::Malformed;
    }
    *offset = start;
    *length = end - start + 1;
    return RangeResult::Ok;
}

// 解析路径中的十进制整数。只接受纯数字串，拒绝 "+12"、" 12"、"0x10" 这类
// 会被 toLongLong 宽松接受的形式，避免与数据库口径不一致。
// 是否必须为正由调用方判定（fileId 从 1 起，而分片序号从 0 起）
bool parsePathNumber(const QString &text, qint64 *out)
{
    *out = 0;
    if (text.isEmpty() || text.size() > 19) {
        return false;
    }
    for (const QChar &c : text) {
        if (!c.isDigit()) {
            return false;
        }
    }
    bool ok = false;
    const qint64 value = text.toLongLong(&ok);
    if (!ok || value < 0) {
        return false;
    }
    *out = value;
    return true;
}

} // namespace

FileHttpService::FileHttpService(QObject *parent, const QString &dbConnectionName)
    : QObject(parent)
    // 独立连接名：路由处理器在主线程执行，不能复用连接线程的数据库实例
    , m_db(dbConnectionName)
{
}

void FileHttpService::setObjectStorage(IObjectStorage *storage)
{
    m_objectStorage = storage;
}

bool FileHttpService::start(quint16 port, const QSslConfiguration &sslConfig, bool tlsEnabled,
                            bool allowPlaintext)
{
    if (!m_objectStorage) {
        // fail-closed：没有存储的数据面只会对每个请求回 500，不如不启动
        qCritical() << "[FileHttp] Object storage is not available; data plane disabled";
        return false;
    }
    if (!m_db.initialize()) {
        qCritical() << "[FileHttp] Database init failed; data plane disabled";
        return false;
    }

    // TLS 优先，且与主通道同一套证书与 CA。明文只在显式开关下允许（开发用）
    m_tls = tlsEnabled;
    if (!m_tls && !allowPlaintext) {
        qCritical() << "[FileHttp] TLS unavailable and plaintext not allowed; refusing to start";
        return false;
    }

    m_httpServer.route("/file/<arg>/chunk/<arg>", QHttpServerRequest::Method::Put,
                       [this](const QString &fileIdText, const QString &indexText,
                              const QHttpServerRequest &request,
                              QHttpServerResponder &responder) {
                           handleChunkPut(fileIdText, indexText, request, responder);
                       });
    m_httpServer.route("/file/<arg>", QHttpServerRequest::Method::Get,
                       [this](const QString &fileIdText, const QHttpServerRequest &request,
                              QHttpServerResponder &responder) {
                           handleBlobGet(fileIdText, request, responder);
                       });

    QTcpServer *server = nullptr;
    if (m_tls) {
        auto *sslServer = new QSslServer(this);
        sslServer->setSslConfiguration(sslConfig);
        server = sslServer;
    } else {
        server = new QTcpServer(this);
    }
    // 顺序不可颠倒：QAbstractHttpServer::bind() 要求 server 已在监听，
    // 否则直接失败并告警 "The TCP server ... is not listening."
    if (!server->listen(QHostAddress::Any, port)) {
        qCritical() << "[FileHttp] Failed to listen on port" << port << ":" << server->errorString();
        delete server;
        return false;
    }
    if (!m_httpServer.bind(server)) {
        qCritical() << "[FileHttp] Failed to bind HTTP server";
        server->close();
        delete server;
        return false;
    }
    m_server = server;
    // port 传 0 时由操作系统分配，此处取实际监听端口；baseUrl() 基于它拼接，
    // 因此下发给客户端的地址依然正确（集成测试靠此避开端口冲突）
    m_port = server->serverPort();

    qInfo() << "[FileHttp] Data plane ready at" << baseUrl()
            << (m_tls ? "(TLS)" : "(PLAINTEXT, development only)");
    StructuredLogger::event(XYChat::Security::LogLevel::Info, "file.http_started")
        .field("port", m_port)
        .field("tls", m_tls)
        .write();
    return true;
}

bool FileHttpService::isListening() const
{
    return m_server != nullptr && m_server->isListening();
}

quint16 FileHttpService::serverPort() const
{
    return m_port;
}

void FileHttpService::setAdvertisedHost(const QString &host)
{
    if (!host.isEmpty()) {
        m_advertisedHost = host;
    }
}

QString FileHttpService::baseUrl() const
{
    if (m_port == 0) {
        return QString();
    }
    const QString scheme = m_tls ? "https" : "http";
    return scheme + "://" + m_advertisedHost + ":" + QString::number(m_port) + "/file";
}

std::optional<FileTicketInfo> FileHttpService::authorizeTicket(const QHttpServerRequest &request,
                                                              qint64 fileId, const QString &kind,
                                                              QHttpServerResponder &responder)
{
    const QByteArray ticket = request.value(TicketHeader);
    if (ticket.isEmpty()) {
        noteAuthFailure(request);
        respondError(responder, Status::Unauthorized, "Missing file ticket");
        return std::nullopt;
    }

    const QString hash = FileCrypto::ticketHash(QString::fromLatin1(ticket));
    const auto info = hash.isEmpty() ? std::optional<FileTicketInfo>()
                                     : m_db.validateFileTicket(hash, kind);
    // 票据与 fileId、用途双向绑定。任一不匹配都回同一个 401：差异化响应会让本端点
    // 变成"某票据是否存在/属于哪个文件"的探测预言机。票据明文绝不进日志
    if (!info.has_value() || info->fileId != fileId) {
        noteAuthFailure(request);
        StructuredLogger::event(XYChat::Security::LogLevel::Warning, "file.http_ticket_denied")
            .field("fileId", fileId)
            .field("kind", kind)
            .field("reason", info.has_value() ? "file_mismatch" : "invalid_or_expired")
            .field("ip", request.remoteAddress().toString())
            .write();
        respondError(responder, Status::Unauthorized, "Invalid file ticket");
        return std::nullopt;
    }
    // 授权即续期（仅下载票据）：单次 GET 有字节上限，2 GiB 文件要 2048 次 Range GET，
    // 固定 TTL 会让慢链路下载中途失效。续期失败不改变本次授权结论（票据此刻已验证
    // 有效），只意味着"若客户端长时间空闲，下次可能需要重新申请票据"——而客户端
    // 收到 401 会重新申请并从断点续传，因此这里绝不能 fail-closed 拒绝本次请求
    if (kind == QLatin1String(Protocol::FileTicketKind::Download)) {
        m_db.renewFileTicket(info->id, Protocol::DownloadTicketTtlSeconds,
                             Protocol::DownloadTicketMaxLifetimeSeconds);
    }
    return info;
}

bool FileHttpService::rejectIfAbusing(const QHttpServerRequest &request,
                                      QHttpServerResponder &responder)
{
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    pruneFailureWindows(now);
    const QString ip = request.remoteAddress().toString();
    if (ip.isEmpty()) {
        return false;
    }
    const auto it = m_failureWindows.constFind(ip);
    if (it == m_failureWindows.constEnd()) {
        return false;
    }
    if (now - it->windowStart < AuthFailureWindowSeconds && it->count >= MaxAuthFailuresPerWindow) {
        respondError(responder, Status::TooManyRequests, "Too many failed requests");
        return true;
    }
    return false;
}

void FileHttpService::noteAuthFailure(const QHttpServerRequest &request)
{
    const QString ip = request.remoteAddress().toString();
    if (ip.isEmpty()) {
        return;
    }
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    AuthFailureWindow &window = m_failureWindows[ip];
    if (now - window.windowStart >= AuthFailureWindowSeconds) {
        window.windowStart = now;
        window.count = 0;
    }
    ++window.count;
    if (m_failureWindows.size() > MaxTrackedAddresses) {
        pruneFailureWindows(now);
    }
}

void FileHttpService::pruneFailureWindows(qint64 nowSecs)
{
    // 未达半数阈值时不做任何遍历：绝大多数请求都走这条快路径
    if (m_failureWindows.size() <= MaxTrackedAddresses / 2) {
        return;
    }
    for (auto it = m_failureWindows.begin(); it != m_failureWindows.end();) {
        if (nowSecs - it->windowStart >= AuthFailureWindowSeconds) {
            it = m_failureWindows.erase(it);
        } else {
            ++it;
        }
    }
    // 仍超阈值（大量并发伪造源地址）则整体清空：宁可短暂放开失败限流，
    // 也不让内存无界增长（与 NonceCache 的容量取舍同理）
    if (m_failureWindows.size() > MaxTrackedAddresses) {
        m_failureWindows.clear();
    }
}

void FileHttpService::handleChunkPut(const QString &fileIdText, const QString &indexText,
                                     const QHttpServerRequest &request,
                                     QHttpServerResponder &responder)
{
    if (rejectIfAbusing(request, responder)) {
        return;
    }

    qint64 fileId = 0;
    qint64 indexValue = 0;
    if (!parsePathNumber(fileIdText, &fileId) || fileId <= 0) {
        noteAuthFailure(request);
        respondError(responder, Status::BadRequest, "Malformed fileId");
        return;
    }
    // 分片序号从 0 起，上限由分片数常量封顶（expectedChunkBytes 会再按该文件的
    // 实际分片口径校一次，此处先拦住荒谬值以免无意义的查询）
    if (!parsePathNumber(indexText, &indexValue) || indexValue >= Protocol::MaxChunkCount) {
        noteAuthFailure(request);
        respondError(responder, Status::BadRequest, "Malformed chunk index");
        return;
    }

    const auto ticket = authorizeTicket(request, fileId, Protocol::FileTicketKind::Upload, responder);
    if (!ticket.has_value()) {
        return;
    }

    const auto rec = m_db.getFileRecord(fileId);
    // 票据有效后，状态类错误对调用方可见（与控制面 requireOwnedFile 之后的分层一致）
    if (!rec.has_value()) {
        respondError(responder, Status::NotFound, "File not found");
        return;
    }
    if (rec->status != Protocol::FileStatus::Uploading) {
        respondError(responder, Status::Conflict, "Upload is not in progress");
        return;
    }

    const int index = static_cast<int>(indexValue);
    const qint64 expected = Protocol::expectedChunkBytes(rec->sizeBytes, rec->chunkSize,
                                                         rec->chunkCount, index);
    if (expected < 0) {
        respondError(responder, Status::BadRequest, "Chunk index out of range");
        return;
    }

    // 早退：声明的长度已超上限就不必再碰 body（QHttpServer 已读入内存，
    // 但至少避免把它复制到存储层与日志）
    const QByteArray body = request.body();
    if (body.size() > Protocol::MaxChunkSize) {
        respondError(responder, Status::PayloadTooLarge, "Chunk exceeds the maximum size");
        return;
    }
    // 长度必须精确匹配：客户端自选分片边界会绕过体积与分片数校验
    if (static_cast<qint64>(body.size()) != expected) {
        QJsonObject obj;
        obj["error"] = "Chunk size does not match the expected layout";
        obj["expectedBytes"] = expected;
        responder.write(QJsonDocument(obj), Status::BadRequest);
        return;
    }

    if (!m_objectStorage->putChunk(rec->blobKey, index, body)) {
        StructuredLogger::event(XYChat::Security::LogLevel::Warning, "file.http_put_failed")
            .userId(ticket->userId)
            .field("fileId", fileId)
            .field("chunkIndex", index)
            .write();
        respondError(responder, Status::InternalServerError, "Failed to store chunk");
        return;
    }

    QJsonObject obj;
    obj["fileId"] = fileId;
    obj["chunkIndex"] = index;
    obj["storedBytes"] = body.size();
    responder.write(QJsonDocument(obj), Status::Ok);
}

void FileHttpService::handleBlobGet(const QString &fileIdText, const QHttpServerRequest &request,
                                    QHttpServerResponder &responder)
{
    if (rejectIfAbusing(request, responder)) {
        return;
    }

    qint64 fileId = 0;
    if (!parsePathNumber(fileIdText, &fileId) || fileId <= 0) {
        noteAuthFailure(request);
        respondError(responder, Status::BadRequest, "Malformed fileId");
        return;
    }

    const auto ticket = authorizeTicket(request, fileId, Protocol::FileTicketKind::Download,
                                        responder);
    if (!ticket.has_value()) {
        return;
    }

    const auto rec = m_db.getFileRecord(fileId);
    if (!rec.has_value()) {
        respondError(responder, Status::NotFound, "File not found");
        return;
    }
    if (rec->status != Protocol::FileStatus::Ready) {
        respondError(responder, Status::Conflict, "File is not ready for download");
        return;
    }

    const qint64 blobSize = m_objectStorage->blobSize(rec->blobKey);
    if (blobSize <= 0) {
        // 元数据说 ready 而对象缺失：属服务端不一致（磁盘故障或回收竞态），
        // 回 409 让客户端重新申请票据/重试，不要谎报 404 让它以为文件从未存在
        StructuredLogger::event(XYChat::Security::LogLevel::Warning, "file.http_blob_missing")
            .userId(ticket->userId)
            .field("fileId", fileId)
            .write();
        respondError(responder, Status::Conflict, "Stored object is unavailable");
        return;
    }

    qint64 offset = 0;
    qint64 length = 0;
    const RangeResult range = parseRange(request.value("Range"), blobSize, &offset, &length);
    if (range == RangeResult::Malformed) {
        respondError(responder, Status::BadRequest, "Malformed Range header");
        return;
    }
    if (range == RangeResult::Unsatisfiable) {
        QHttpHeaders headers;
        headers.append(QHttpHeaders::WellKnownHeader::ContentRange,
                       QLatin1String("bytes */") + QString::number(blobSize));
        responder.write(headers, Status::RequestRangeNotSatisfiable);
        return;
    }

    // 无 Range 时按单次上限拒绝超大对象：客户端本就按分片解密，分段取即可
    qint64 readOffset = offset;
    qint64 readLength = length;
    bool partial = range == RangeResult::Ok;
    if (!partial) {
        if (blobSize > MaxSingleGetBytes) {
            QHttpHeaders headers;
            headers.append(QHttpHeaders::WellKnownHeader::AcceptRanges, "bytes");
            headers.append(QHttpHeaders::WellKnownHeader::ContentType, DownloadMimeType);
            headers.append("X-XYChat-Total-Size", QString::number(blobSize));
            responder.write(headers, Status::PayloadTooLarge);
            return;
        }
        readOffset = 0;
        readLength = blobSize;
    } else if (readLength > MaxSingleGetBytes) {
        respondError(responder, Status::PayloadTooLarge, "Requested range is too large");
        return;
    }

    const QByteArray data = m_objectStorage->readRange(rec->blobKey, readOffset, readLength);
    if (data.size() != readLength) {
        // 读到的字节数与请求不符（并发回收或磁盘故障）：不得把截断数据当成功返回，
        // 否则客户端的整体 SHA-256 校验会失败却无法定位原因
        StructuredLogger::event(XYChat::Security::LogLevel::Warning, "file.http_read_short")
            .userId(ticket->userId)
            .field("fileId", fileId)
            .field("requested", readLength)
            .field("actual", data.size())
            .write();
        respondError(responder, Status::InternalServerError, "Failed to read stored object");
        return;
    }

    QHttpHeaders headers;
    headers.append(QHttpHeaders::WellKnownHeader::ContentType, DownloadMimeType);
    headers.append(QHttpHeaders::WellKnownHeader::AcceptRanges, "bytes");
    headers.append(QHttpHeaders::WellKnownHeader::CacheControl, "no-store");
    headers.append("X-XYChat-Total-Size", QString::number(blobSize));
    if (partial) {
        const QString contentRange = QLatin1String("bytes ")
            + QString::number(readOffset) + QLatin1String("-")
            + QString::number(readOffset + readLength - 1) + QLatin1String("/")
            + QString::number(blobSize);
        headers.append(QHttpHeaders::WellKnownHeader::ContentRange, contentRange);
    }
    responder.write(data, headers, partial ? Status::PartialContent : Status::Ok);
}

} // namespace XYChat::Server
