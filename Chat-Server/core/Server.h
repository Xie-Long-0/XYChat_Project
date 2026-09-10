#pragma once

#include <QObject>
#include <QTcpServer>
#include <QSslConfiguration>
#include <QHash>
#include <QSet>

#include <memory>

#include "NonceCache.h"
#include "database/DatabaseManager.h"
#include "storage/IObjectStorage.h"

class RequestHandler;
class QTimer;

class ConnectionServer : public QTcpServer
{
    Q_OBJECT

public:
    using QTcpServer::QTcpServer;

signals:
    void socketAccepted(qintptr socketDescriptor);

protected:
    void incomingConnection(qintptr socketDescriptor) override;
};

class Server : public QObject
{
    Q_OBJECT

public:
    explicit Server(QObject *parent = nullptr);
    // M5.5: fail-closed：若 TLS 未启用且未显式允许明文，start() 拒绝启动
    bool start(quint16 port, bool allowPlaintext = false);

    // M5: TLS 配置
    bool initTls(const QString &certDir);

    // M5.5: 是否处于 TLS 保护状态（供测试与监控）
    bool tlsEnabled() const { return m_tlsEnabled; }

    // M8: 覆盖对象存储根目录（默认 <GenericData>/XYChat-Server/data/files），
    // 须在 start() 前调用
    void setStorageRoot(const QString &rootPath);
    // M8: 对象存储是否就绪（未就绪时文件类请求一律 fail-closed）
    bool storageReady() const { return m_objectStorage != nullptr; }

    // 在线用户管理
    int onlineUserCount() const;
    QSet<qint64> onlineUserIds() const;

private slots:
    void onSocketAccepted(qintptr socketDescriptor);
    void onUserLoggedIn(qint64 userId, qint64 sessionId, const QString &deviceId);
    void onUserLoggedOut(qint64 userId, qint64 sessionId);
    void onHandlerFinished();
    // M3: 消息路由
    void onMessageForUser(qint64 targetUserId, const QByteArray &packetData);
    // M5.5: 本人会话被终止时断开对应连接
    void onSessionTerminated(qint64 sessionId);

private:
    // M8: 回收超期未完成的上传与过期票据（与 sync_events 清理共用维护连接与定时器）
    void pruneFileUploads();

    ConnectionServer *tcpServer;

    // M5: TLS 配置
    QSslConfiguration m_sslConfig;
    bool m_tlsEnabled = false;

    // M5.5: 全局 nonce 缓存（各连接共享，TTL 去重）
    NonceCache m_nonceCache;

    // M8: 对象存储（本地文件系统实现，各连接共享）。初始化失败时保持为空，
    // handler 据此对文件请求 fail-closed，不拖垮纯文本聊天能力
    QString m_storageRoot;
    std::unique_ptr<XYChat::Server::IObjectStorage> m_objectStorage;

    // userId -> set of sessionIds
    QHash<qint64, QSet<qint64>> m_onlineSessions;
    // sessionId -> handler
    QHash<qint64, RequestHandler *> m_sessionHandlers;
    // handler -> userId (for cleanup)
    QHash<RequestHandler *, qint64> m_handlerUsers;

    // M9: sync_events 保留清理——独立维护连接 + 定时器（全局单例，非 per-connection）
    DatabaseManager m_maintenanceDb;
    QTimer *m_pruneTimer = nullptr;
    static constexpr int SyncEventRetentionDays = 30;       // 事件保留期（天）
    static constexpr int PruneIntervalMs = 60 * 60 * 1000;  // 清理周期（每小时）
};
