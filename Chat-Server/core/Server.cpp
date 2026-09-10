#include "Server.h"
#include "RequestHandler.h"
#include "TlsHelper.h"
#include "StructuredLogger.h"
#include "storage/LocalFileStorage.h"
#include "protocol/FileProtocol.h"

#include <QStandardPaths>
#include <QTimer>

using XYChat::Security::StructuredLogger;
using XYChat::Security::LogLevel;

namespace
{
// M8: 对象存储默认根目录，与 DatabaseManager 的数据目录同一基准
// （<GenericData>/XYChat-Server/data/），使部署时只需挂一个数据盘
QString defaultStorageRoot()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
        + "/XYChat-Server/data/files";
}
} // namespace

void Server::setStorageRoot(const QString &rootPath)
{
    m_storageRoot = rootPath;
}

void ConnectionServer::incomingConnection(qintptr socketDescriptor)
{
    emit socketAccepted(socketDescriptor);
}

Server::Server(QObject *parent)
    : QObject(parent)
    , tcpServer(new ConnectionServer(this))
    , m_maintenanceDb("server_maintenance")
{
    connect(tcpServer, &ConnectionServer::socketAccepted, this, &Server::onSocketAccepted);
}

bool Server::initTls(const QString &certDir)
{
    using namespace XYChat::Security;

    const QString certPath = certDir + "/server.crt";
    const QString keyPath = certDir + "/server.key";
    const QString caCertPath = certDir + "/ca.crt";

    TlsHelper::TlsConfig tlsConfig = TlsHelper::loadServerConfig(
        certPath, keyPath, caCertPath, true /* allowGenerate */);

    if (!tlsConfig.valid) {
        qCritical() << "[Server] TLS initialization failed";
        return false;
    }

    m_sslConfig = QSslConfiguration::defaultConfiguration();
    m_sslConfig.setLocalCertificate(tlsConfig.certificate);
    m_sslConfig.setPrivateKey(tlsConfig.privateKey);
    m_sslConfig.setProtocol(QSsl::TlsV1_2OrLater);
    m_tlsEnabled = true;

    qInfo() << "[Server] TLS enabled (TLS 1.2+)";
    return true;
}

bool Server::start(quint16 port, bool allowPlaintext)
{
    // M5.5: fail-closed：TLS 不可用时拒绝启动，
    // 除非显式开启开发明文模式（默认关闭）
    if (!m_tlsEnabled && !allowPlaintext) {
        qCritical() << "[Server] TLS is not available; refusing to start in plaintext."
                    << "Fix TLS configuration or pass --allow-plaintext for development only.";
        return false;
    }
    if (!m_tlsEnabled && allowPlaintext) {
        qWarning() << "[Server] Starting in PLAINTEXT development mode. Do not use in production.";
    }

    // M8: 初始化对象存储。失败只关闭文件能力（handler 对文件请求 fail-closed），
    // 不拖垮整个服务端：消息收发与会话管理与对象存储无关
    {
        const QString root = m_storageRoot.isEmpty() ? defaultStorageRoot() : m_storageRoot;
        auto storage = std::make_unique<XYChat::Server::LocalFileStorage>(root);
        if (storage->initialize()) {
            m_objectStorage = std::move(storage);
            qInfo() << "[Server] Object storage ready at" << root;
        } else {
            qCritical() << "[Server] Failed to initialize object storage at" << root
                        << "; file transfer is disabled";
        }
    }

    // M9: 初始化维护连接并启动 sync_events 定时清理（启动即清理一次 + 每小时）
    if (m_maintenanceDb.initialize()) {
        m_maintenanceDb.pruneSyncEvents(SyncEventRetentionDays);
        // M8: 同一轮顺手回收超期上传与过期票据，不另起定时器
        pruneFileUploads();
        m_pruneTimer = new QTimer(this);
        connect(m_pruneTimer, &QTimer::timeout, this, [this]() {
            m_maintenanceDb.pruneSyncEvents(SyncEventRetentionDays);
            pruneFileUploads();
        });
        m_pruneTimer->start(PruneIntervalMs);
    } else {
        qWarning() << "[Server] Maintenance DB init failed; sync_events pruning disabled";
    }

    if (tcpServer->listen(QHostAddress::Any, port)) {
        qDebug() << "[Server] Listening on port" << port
                 << (m_tlsEnabled ? "(TLS)" : "(plain TCP, dev mode)");
        return true;
    }
    qDebug() << "[Server] Failed to listen on port" << port << tcpServer->errorString();
    return false;
}

void Server::onSocketAccepted(qintptr socketDescriptor)
{
    RequestHandler *handler = new RequestHandler(socketDescriptor, this);

    // M5: 传递 TLS 配置
    if (m_tlsEnabled) {
        handler->setSslConfiguration(m_sslConfig);
    }
    // M5.5: 传递全局 nonce 缓存
    handler->setNonceCache(&m_nonceCache);
    // M8: 传递对象存储（未就绪时为 nullptr，handler 对文件请求 fail-closed）
    handler->setObjectStorage(m_objectStorage.get());

    connect(handler, &RequestHandler::userLoggedIn, this, &Server::onUserLoggedIn);
    connect(handler, &RequestHandler::userLoggedOut, this, &Server::onUserLoggedOut);
    connect(handler, &RequestHandler::finished, this, &Server::onHandlerFinished);
    connect(handler, &RequestHandler::finished, handler, &RequestHandler::deleteLater);
    // M3: 消息路由
    connect(handler, &RequestHandler::messageForUser, this, &Server::onMessageForUser);
    // M5.5: 会话终止路由
    connect(handler, &RequestHandler::sessionTerminated, this, &Server::onSessionTerminated);

    handler->start();
}

void Server::onUserLoggedIn(qint64 userId, qint64 sessionId, const QString &deviceId)
{
    Q_UNUSED(deviceId);
    RequestHandler *handler = qobject_cast<RequestHandler *>(sender());
    if (!handler) return;

    m_onlineSessions[userId].insert(sessionId);
    m_sessionHandlers[sessionId] = handler;
    m_handlerUsers[handler] = userId;

    StructuredLogger::event(LogLevel::Info, "session.online")
        .userId(userId).field("sessionId", sessionId)
        .field("onlineUsers", onlineUserCount()).write();
}

void Server::onUserLoggedOut(qint64 userId, qint64 sessionId)
{
    if (sessionId == 0) {
        // 强制下线：清除该用户所有 session
        m_onlineSessions.remove(userId);
        auto it = m_handlerUsers.begin();
        while (it != m_handlerUsers.end()) {
            if (it.value() == userId) {
                it = m_handlerUsers.erase(it);
            } else {
                ++it;
            }
        }
        StructuredLogger::event(LogLevel::Info, "session.offline")
            .userId(userId).field("reason", "all_sessions_cleared").write();
    } else {
        m_onlineSessions[userId].remove(sessionId);
        if (m_onlineSessions[userId].isEmpty()) {
            m_onlineSessions.remove(userId);
        }
        m_sessionHandlers.remove(sessionId);
        StructuredLogger::event(LogLevel::Info, "session.offline")
            .userId(userId).field("sessionId", sessionId).write();
    }
}

void Server::onHandlerFinished()
{
    RequestHandler *handler = qobject_cast<RequestHandler *>(sender());
    if (!handler) return;

    auto it = m_handlerUsers.find(handler);
    if (it != m_handlerUsers.end()) {
        const qint64 userId = it.value();
        m_handlerUsers.erase(it);

        if (m_onlineSessions.contains(userId)) {
            auto sit = m_sessionHandlers.begin();
            while (sit != m_sessionHandlers.end()) {
                if (sit.value() == handler) {
                    m_onlineSessions[userId].remove(sit.key());
                    sit = m_sessionHandlers.erase(sit);
                } else {
                    ++sit;
                }
            }
            if (m_onlineSessions[userId].isEmpty()) {
                m_onlineSessions.remove(userId);
            }
        }
    }

    StructuredLogger::event(LogLevel::Info, "handler.finished")
        .field("onlineUsers", onlineUserCount()).write();
}

int Server::onlineUserCount() const
{
    return m_onlineSessions.size();
}

QSet<qint64> Server::onlineUserIds() const
{
    QSet<qint64> result;
    for (auto it = m_onlineSessions.begin(); it != m_onlineSessions.end(); ++it) {
        result.insert(it.key());
    }
    return result;
}

// M3: 消息路由
void Server::onMessageForUser(qint64 targetUserId, const QByteArray &packetData)
{
    // 查找目标用户的所有在线 handler
    auto it = m_onlineSessions.find(targetUserId);
    if (it == m_onlineSessions.end()) {
        // 用户不在线，消息已存储在数据库中，用户上线后可通过 sync 获取
        return;
    }

    const QSet<qint64> sessionIds = it.value();
    for (qint64 sessionId : sessionIds) {
        auto handlerIt = m_sessionHandlers.find(sessionId);
        if (handlerIt != m_sessionHandlers.end()) {
            handlerIt.value()->sendRawData(packetData);
        }
    }
}

// M5.5: 本人其他会话被 terminate_session 终止时，断开对应连接
void Server::onSessionTerminated(qint64 sessionId)
{
    auto handlerIt = m_sessionHandlers.find(sessionId);
    if (handlerIt != m_sessionHandlers.end()) {
        handlerIt.value()->disconnectClient();
        StructuredLogger::event(LogLevel::Info, "session.force_disconnect")
            .field("sessionId", sessionId).write();
    }
}

// M8: 回收超期未完成的上传、终态行与过期票据
//
// 上传中而始终未宣告完成的文件会一直占着磁盘与并发配额（配额满后用户
// 再也发不了文件），因此必须主动回收。与 sync_events 清理共用维护连接与定时器，
// 不另起后台线程。
//
// 前两轮真正删数据，顺序上都是"先幂等删磁盘、再落元数据"：反序会产生无索引
// 指向的孤儿数据。取消接口同样遵循这一顺序，因此它失败时留下的残留会在第二轮被清掉
void Server::pruneFileUploads()
{
    const int ticketsPruned = m_maintenanceDb.pruneExpiredFileTickets();
    if (ticketsPruned > 0) {
        StructuredLogger::event(LogLevel::Info, "file.tickets_pruned")
            .field("count", ticketsPruned).write();
    }

    if (!m_objectStorage) {
        // 存储不可用时不动元数据：删行会把磁盘上的分片变成永久孤儿
        return;
    }

    // 第一轮：超期未完成的上传（status 仍为 uploading）
    const QList<FileRecord> stale =
        m_maintenanceDb.getStaleUploads(XYChat::Protocol::StaleUploadHours);
    for (const FileRecord &rec : stale) {
        if (!m_objectStorage->remove(rec.blobKey)) {
            // 删不掉就保留 uploading 状态，下一轮重试；先改状态会让磁盘数据无人认领
            StructuredLogger::event(LogLevel::Warning, "file.stale_reap_failed")
                .userId(rec.uploaderId).field("fileId", rec.id).write();
            continue;
        }
        if (m_maintenanceDb.markFileCancelled(rec.id)) {
            StructuredLogger::event(LogLevel::Info, "file.stale_reaped")
                .userId(rec.uploaderId).field("fileId", rec.id).write();
        }
        // 状态迁移失败意味着已被并发取消，数据已回收，不重复上报
    }

    // 第二轮：终态行（cancelled/failed）的收尾。既兼作孤儿数据清理（取消时删盘
    // 失败、或 finalize 后 markFileReady 失败又被回收任务取消的 blob），
    // 也防止 files 表随时间无界增长
    const QList<FileRecord> terminal =
        m_maintenanceDb.getTerminalFiles(XYChat::Protocol::StaleUploadHours);
    for (const FileRecord &rec : terminal) {
        // 纵深防御：终态行通常不可能被未删除消息引用（发送校验要求 status=ready，
        // 且插入语句内已原子复核），但校验与写入之间仍有跨线程窗口。删盘前先
        // 判引用：宁可留下一条指向仍在盘上对象的 cancelled 行（可修复），
        // 也不销毁仍被引用的数据（不可恢复）
        if (m_maintenanceDb.isFileReferencedByMessage(rec.id)) {
            StructuredLogger::event(LogLevel::Warning, "file.terminal_still_referenced")
                .userId(rec.uploaderId).field("fileId", rec.id)
                .field("status", rec.status).write();
            continue;
        }
        // remove 幂等：磁盘上已无数据也返回成功
        if (!m_objectStorage->remove(rec.blobKey)) {
            StructuredLogger::event(LogLevel::Warning, "file.terminal_cleanup_failed")
                .userId(rec.uploaderId).field("fileId", rec.id).write();
            continue;
        }
        // 被未删除消息引用的行不得删除（deleteFileRecord 内已做双重保险）
        if (m_maintenanceDb.deleteFileRecord(rec.id)) {
            StructuredLogger::event(LogLevel::Info, "file.terminal_reaped")
                .userId(rec.uploaderId).field("fileId", rec.id)
                .field("status", rec.status).write();
        }
    }

    // 第三轮：已就绪但引用它的消息已被软删除（或上传完成后发送始终未发生）
    // 的文件。本轮只把行迁入终态，不直接碰磁盘：引用判定与迁移在同一条语句
    // 内原子完成，销毁推到下一轮的第二步（那里删盘前会再判一次引用）。
    //
    // 注意“终态行不可能再被引用”并不成立：发送侧的文件校验与消息写入存在
    // 跨线程窗口，因此两边都做了防护——发送侧把“文件仍为 ready”下推为
    // INSERT 的守卫子查询（单语句原子，写者串行），回收侧删盘前再判引用。
    // 把销毁推到下一轮也留出一个维护周期的观察窗口
    const QList<FileRecord> orphaned =
        m_maintenanceDb.getUnreferencedReadyFiles(XYChat::Protocol::StaleUploadHours);
    for (const FileRecord &rec : orphaned) {
        if (m_maintenanceDb.cancelUnreferencedReadyFile(rec.id)) {
            StructuredLogger::event(LogLevel::Info, "file.orphaned_marked")
                .userId(rec.uploaderId).field("fileId", rec.id)
                .field("sizeBytes", rec.sizeBytes).write();
        }
        // 未迁移：期间有新消息引用了它（正常业务）或 SQL 失败（下一轮重试）
    }
}
