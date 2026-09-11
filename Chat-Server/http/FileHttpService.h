#pragma once

#include <QHash>
#include <QHttpServer>
#include <QObject>
#include <QString>

#include <optional>

#include "database/DatabaseManager.h"
#include "storage/IObjectStorage.h"

class QSslConfiguration;
class QTcpServer;

namespace XYChat::Server
{

// M8.2: 文件传输数据面（HTTP(S)）
//
// 职责边界：只搬字节与校验票据，不含任何业务逻辑。控制面（申请上传、查询已收
// 分片、宣告完成、取消、申请下载票据）仍在 TCP 主通道，本服务只提供两个端点：
//   PUT /file/<fileId>/chunk/<index>   上传一个密文分片
//   GET /file/<fileId>                 下载密文（支持 Range 分段）
// 断点续传所需的"服务端已收哪些分片"由控制面 92/93 提供，此处不重复。
//
// 授权只认票据：HTTP 层没有会话上下文，请求头 X-XYChat-Ticket 携带明文票据，
// 服务端只比对 SHA-256 摘要（与 session token 同一做法，库泄露不等于凭据泄露）。
// 票据绝不放 URL query：query 会进反向代理与访问日志，等于把凭据写进日志。
//
// 隐私边界：下载恒按 application/octet-stream 投递。服务端不知道真实 MIME
// 与文件名（它们只在客户端加密的清单里），因此也不得猜测或回填。
//
// 线程模型：QHttpServer 的路由处理器在其所属线程（Server 主线程）执行，
// 故本服务持有独立的 DatabaseManager 连接名，不复用连接线程的实例。
class FileHttpService : public QObject
{
    Q_OBJECT

public:
    // dbConnectionName 可注入：路由处理器在主线程执行，生产用专用的
    // "file_http" 连接（不复用连接线程的实例）；DatabaseManager 析构会
    // removeDatabase，因此多实例共存时（如集成测试）必须用不同连接名
    explicit FileHttpService(QObject *parent = nullptr,
                             const QString &dbConnectionName = "file_http");

    // 注入对象存储（须在 start() 前）。存储为空时拒绝启动：数据面没有存储
    // 就毫无意义，必须以 fail-closed 拒绝启动，而不是起来后对每个请求回 500
    void setObjectStorage(IObjectStorage *storage);

    // 启动数据面。tlsEnabled 为真时以 QSslServer 承载（与主通道同一套证书与
    // CA）；TLS 不可用且未显式允许明文时拒绝启动（与主通道 fail-closed 口径一致）
    bool start(quint16 port, const QSslConfiguration &sslConfig, bool tlsEnabled,
               bool allowPlaintext);

    bool isListening() const;
    quint16 serverPort() const;

    // 写入登录响应 fileTransferBaseUrl 的地址。服务端无法自知 NAT/反向代理后的
    // 对外地址，因此主机名由运维显式指定（默认 127.0.0.1，与开发期客户端一致）
    void setAdvertisedHost(const QString &host);
    QString baseUrl() const;

    // 单次 GET 的字节上限。客户端必须按分片边界用 Range 分段取：
    // 分片是独立 AEAD 加密的，客户端本来就只能逐片解密，全量下载并无用处，
    // 而无上限的 readRange 会让单个请求把整个文件（最大 2 GiB）读进内存
    static constexpr qint64 MaxSingleGetBytes = 4 * 1024 * 1024;

private:
    // 路由处理器（fileId/index 以字符串捕获后自行解析：QHttpServerRouter 的
    // 默认 <arg> 只匹配非斜杠串，自行解析才能对畸形输入回 400 而不是 500）
    void handleChunkPut(const QString &fileIdText, const QString &indexText,
                        const QHttpServerRequest &request, QHttpServerResponder &responder);
    void handleBlobGet(const QString &fileIdText, const QHttpServerRequest &request,
                       QHttpServerResponder &responder);

    // 校验票据并确认它绑定到该 fileId 与用途。失败一律回 401，不区分"票据不存在"
    // "已过期""类型不符""fileId 不匹配"：差异化响应会让票据端点变成探测预言机。
    // 票据有效即证明调用方是该文件的上传者或有权下载者，此后的状态类错误
    // （404/409）才对其可见，与控制面 requireOwnedFile 的分层同理
    std::optional<FileTicketInfo> authorizeTicket(const QHttpServerRequest &request,
                                                  qint64 fileId, const QString &kind,
                                                  QHttpServerResponder &responder);

    // per-IP 失败限流：只统计授权失败与畸形请求，不统计成功的数据搬运。
    // 理由：合法上传一个大文件需要多达 MaxChunkCount(4096) 次 PUT，按请求数限流
    // 会直接挡住正常业务；真正的攻击面是票据爆破与未授权扫描，而成功请求的
    // 资源消耗已由控制面的并发配额（8）与单文件上限（2 GiB）封顶
    bool rejectIfAbusing(const QHttpServerRequest &request, QHttpServerResponder &responder);
    void noteAuthFailure(const QHttpServerRequest &request);
    void pruneFailureWindows(qint64 nowSecs);

    QHttpServer m_httpServer;
    QTcpServer *m_server = nullptr;
    IObjectStorage *m_objectStorage = nullptr;
    DatabaseManager m_db;

    QString m_advertisedHost = "127.0.0.1";
    quint16 m_port = 0;
    bool m_tls = false;

    // per-IP 授权失败计数（固定窗口）。不复用 RateWindow：它的 allow() 会递增计数，
    // 而本处需要在不消费额度的前提下窥视当前窗口是否已超限
    struct AuthFailureWindow
    {
        qint64 windowStart = 0;
        int count = 0;
    };
    QHash<QString, AuthFailureWindow> m_failureWindows;
    static constexpr int MaxAuthFailuresPerWindow = 30;
    static constexpr int AuthFailureWindowSeconds = 60;
    static constexpr int MaxTrackedAddresses = 4096;
};

} // namespace XYChat::Server
