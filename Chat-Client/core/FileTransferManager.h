#pragma once

#include <QByteArray>
#include <QFile>
#include <QHash>
#include <QNetworkRequest>
#include <QObject>
#include <QSet>
#include <QSslConfiguration>
#include <QString>

#include "FileProtocol.h"

class QNetworkAccessManager;
class QNetworkReply;
// 全局命名空间的测试类：friend 声明需先有声明，且必须用 :: 限定，
// 否则会被当成本命名空间内的同名类而失效
class TestNetworkManager;

namespace XYChat::Client
{

// M8.2: 文件上传/下载引擎
//
// 职责边界：本类只负责"把本地文件变成服务端 blob"与"把服务端 blob 变回本地
// 文件"，包含分片加解密、断点续传、进度、取消与重试。它不碰 TCP 控制面
// （90-99 属 NetworkManager），也不碰消息 E2EE（清单的加密封装属 NetworkManager）。
//
// 与控制面的协作方式：本类 emit 请求信号（带自增 seq），NetworkManager 执行 TCP
// 请求并在响应到达后调用对应的 onXxx 回调（原样带回 seq）。这样本类不依赖
// requestId 的分配细节，也无需持有 socket，可完全脱离网络做单元测试。
//
// 隐私边界：清单明文（含 32 字节文件密钥）只在本类与 NetworkManager 之间的
// C++ 信号里流转，绝不暴露给 QML。QML 只见任务 token、进度、以及脱敏后的
// 展示字段（文件名/大小/密文摘要）。
//
// 缓存策略：下载得到的是客户端加密的密文，直接原样落盘即可（零额外加密开销，
// 磁盘上天然不是明文），文件密钥只存在于清单中，而清单经 LocalStore 存储密钥
// 加密落库。缓存文件名取密文 SHA-256（协议已有该摘要，一份值同时充当缓存键、
// 完整性校验值与不可预测文件名），无后缀、前两位分 256 桶。明文只在用户显式
// "保存到本地"时写出。
class FileTransferManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int activeTaskCount READ activeTaskCount NOTIFY tasksChanged)
    Q_PROPERTY(bool enabled READ isEnabled NOTIFY baseUrlChanged)

public:
    explicit FileTransferManager(QObject *parent = nullptr);

    // 缓存根目录（默认 <AppData>/XYChat/filecache），须在首次传输前设定
    void setCacheRoot(const QString &path);
    QString cacheRoot() const;

    // 数据面基地址（NetworkManager 从登录响应的 fileTransferBaseUrl 转交）。
    // 为空表示服务端未开启文件能力，此时任务立即失败而不是静默排队
    void setBaseUrl(const QString &url);
    QString baseUrl() const;
    bool isEnabled() const;

    // 数据面 TLS 配置（复用客户端主通道已加载的开发 CA 与校验口径）。
    // 未设置时 https 请求会因自签证书全部失败，故须在首次传输前由
    // NetworkManager 转交（与主通道 fail-closed 口径一致，不在此处放宽校验）
    void setSslConfiguration(const QSslConfiguration &config);

    int activeTaskCount() const;

    // 缓存查询与清理（QML 设置页与文件气泡可用，故标 Q_INVOKABLE）
    Q_INVOKABLE bool isCached(const QString &sha256Hex, qint64 cipherSize) const;
    Q_INVOKABLE qint64 cacheBytes() const;
    // 已登记清单的消息是否已就绪（缓存命中或下载完成）
    Q_INVOKABLE bool isMessageFileAvailable(qint64 messageId) const;

public slots:
    // 上传本地文件并在完成后请求发送。返回任务 token（参数非法也返回 token，
    // 便于 UI 统一挂进度并收到 taskFailed）
    QString uploadAndSend(const QString &localPath, qint64 conversationId, qint64 peerUserId);
    // 取消任务：上传中会一并请求控制面取消（回收服务端已收分片）
    void cancelTask(const QString &token);
    // 下载收到的文件消息（清单须已由 registerIncomingFile 登记）
    QString download(qint64 messageId);
    // 把已缓存的文件解密保存到用户指定路径（明文只在此处落盘）
    bool saveToFile(qint64 messageId, const QString &destPath);
    // 清理全部缓存，返回删除条数（缓存只是副本，清理后可重新下载）
    int clearCache();
    // 登出/断线时重置会话态：中止在途任务并清零已登记的清单密钥。
    // 清单含文件密钥，不得跨会话驻留（与 NetworkManager 的 resetAuthState 同时机）
    void reset();

    // NetworkManager 解密文件消息后登记清单（含密钥，仅内存态）。重启后由
    // NetworkManager 从持久化解密缓存重新解密并再次登记，故无需单独持久化
    void registerIncomingFile(qint64 messageId, const QString &manifestJson);

    // 控制面回调（seq 原样带回，与请求信号一一对应）
    void onUploadCreated(qint64 seq, bool ok, qint64 fileId, const QString &ticket,
                         const QString &error);
    void onUploadQueried(qint64 seq, bool ok, const QList<int> &receivedChunks,
                         const QString &error);
    void onUploadCompleted(qint64 seq, bool ok, const QString &error);
    void onUploadCancelled(qint64 seq, bool ok, const QString &error);
    void onDownloadTicket(qint64 seq, bool ok, const QString &ticket, qint64 sizeBytes,
                          qint64 chunkSize, int chunkCount, const QString &sha256Hex,
                          const QString &error);

signals:
    // 请求 NetworkManager 执行控制面调用
    void uploadCreateRequested(qint64 seq, qint64 cipherSize, qint64 chunkSize, int chunkCount,
                               const QString &sha256Hex);
    void uploadQueryRequested(qint64 seq, qint64 fileId);
    void uploadCompleteRequested(qint64 seq, qint64 fileId);
    void uploadCancelRequested(qint64 seq, qint64 fileId);
    void downloadTicketRequested(qint64 seq, qint64 fileId);
    // 上传完成：请求把清单作为消息正文经既有 E2EE 加密后发送（携带 fileId）。
    // 清单明文只经此 C++ 信号流转，不进 QML
    void sendMessageRequested(qint64 conversationId, qint64 peerUserId,
                              const QString &manifestJson, qint64 fileId);

    void baseUrlChanged();
    void tasksChanged();
    // phase: hashing/uploading/completing/saving 等短标识，供 UI 显示阶段文案
    void taskProgress(QString token, QString phase, qint64 done, qint64 total);
    void taskFinished(QString token);
    void taskFailed(QString token, QString error);
    // state: available/downloading/missing，供文件气泡切换按钮
    void downloadStateChanged(qint64 messageId, QString state);
    void fileSaved(qint64 messageId, QString path);
    void cacheCleared(int removedCount);

private:
    // 供 TestNetworkManager 直接断言"清单与文件密钥确实留在 C++ 侧"
    //（与 NetworkManager 的 friend 注入同一范式）
    friend class ::TestNetworkManager;

    struct Task
    {
        QString token;
        bool isUpload = true;
        bool cancelled = false;

        // 上传侧
        QString localPath;
        qint64 conversationId = 0;
        qint64 peerUserId = 0;
        qint64 plainSize = 0;
        qint64 cipherSize = 0;
        qint64 chunkSize = XYChat::Protocol::DefaultChunkSize;
        int chunkCount = 0;
        QString sha256Hex;
        QString fileName;
        QString mime;
        // M8.3: 图片元数据与内联缩略图（提取失败留空，不影响传输）
        int mediaWidth = 0;
        int mediaHeight = 0;
        QByteArray thumbnail;
        QByteArray fileKey;  // 32B，用后清零
        QByteArray iv;       // 12B nonce 前缀
        qint64 fileId = 0;
        QString uploadTicket;
        int nextChunk = 0;
        QSet<int> remainingChunks;

        // 下载侧
        qint64 messageId = 0;
        QString downloadTicket;
        int downloadIndex = 0;
        QString tmpPath;  // 下载中的临时密文文件

        // 当前正在传输的分片（上传/下载共用）与本分片已重试次数。
        // 串行传输下只有一个在途分片，故无需 per-chunk 计数
        int activeIndex = -1;
        int attempts = 0;
        // 恢复轮次（每次"PUT 失败 -> 查询已收分片"计一轮）。必须与 attempts
        // 分开：数据面持续 5xx 而控制面正常时，查询总是成功，若用它清零
        // 重试预算就会形成"上传-失败-查询-重上传"的活锁
        int recoveryRounds = 0;

        qint64 bytesDone = 0;
        QString phase;
    };

    qint64 nextSeq();
    // 为本任务分配一个 seq 并记下 seq -> token 的映射（回调时反查）
    qint64 requestSeq(const QString &token);
    QString makeToken();

    // 第一遍：流式加密以计算密文整体摘要（不落临时文件，代价是多一遍 AES 运算，
    // 换来的是磁盘占用不翻倍、也没有崩溃残留需要清理）
    bool computeCipherDigest(Task &task, QString *error);
    void pumpUpload(Task &task);
    void pumpDownload(Task &task);
    // 串行调度：一次只跑一个分片（避免带宽争抢、内存峰值与 per-IP 限流），
    // 当前分片完成后推进下一个可运行的任务
    void pumpNext();
    void onPutFinished(Task &task, QNetworkReply *reply);
    void onGetFinished(Task &task, QNetworkReply *reply);
    // 下载收尾：校验整体摘要后原子改名进缓存（不通过则丢弃临时文件）
    bool finalizeDownload(Task &task, QString *error);
    QNetworkRequest makeRequest(const QUrl &url) const;

    QString cachePathFor(const QString &sha256Hex) const;
    bool ensureCacheRoot();
    void finishTask(const QString &token);
    void failTask(const QString &token, const QString &error);
    Task *taskBySeq(qint64 seq);

    QNetworkAccessManager *m_nam = nullptr;
    QNetworkReply *m_activeReply = nullptr;  // 串行传输：一次只跑一片
    QString m_activeToken;

    // 重入护栏：pumpNext 的循环体内可能同步走到 failTask/finishTask（它们
    // 会 erase 任务并递归推进），不拦住就会在遍历中销毁当前节点并嵌套泵送
    bool m_pumping = false;
    // reset 护栏：abort() 会同步触发 finished 回调，若此时任务表未清空，
    // 回调会在登出中途发起新请求并向 UI 弹失败
    bool m_resetting = false;

    QHash<QString, Task> m_tasks;
    QHash<qint64, QString> m_seqToToken;
    QHash<qint64, XYChat::Protocol::FileManifest> m_incoming;  // messageId -> 清单
    qint64 m_nextSeqValue = 1;

    QString m_baseUrl;
    QString m_cacheRoot;
    QSslConfiguration m_sslConfig;
};

} // namespace XYChat::Client
