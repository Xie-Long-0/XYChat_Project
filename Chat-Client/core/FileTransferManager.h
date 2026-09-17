#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QMutex>
#include <QNetworkRequest>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QSslConfiguration>
#include <QString>
#include <QUrl>
#include <QVariant>

#include "FileProtocol.h"
#include "MediaMetadataExtractor.h"

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;
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
    // P4.1: 活动任务快照（token/fileName/phase/isUpload/messageId），供 UI 以
    // Repeater 渲染多任务传输横幅。相位/进度经 taskProgress 增量更新，本属性
    // 仅在任务增删（tasksChanged）时刷新条目集合
    Q_PROPERTY(QVariantList tasks READ tasks NOTIFY tasksChanged)
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
    // P4.1: 活动任务快照（见 tasks 属性）
    QVariantList tasks() const;

    // 缓存查询与清理（QML 设置页与文件气泡可用，故标 Q_INVOKABLE）
    Q_INVOKABLE bool isCached(const QString &sha256Hex, qint64 cipherSize) const;
    Q_INVOKABLE qint64 cacheBytes() const;
    // 已登记清单的消息是否已就绪（缓存命中或下载完成）
    Q_INVOKABLE bool isMessageFileAvailable(qint64 messageId) const;

    // 把 QML 传来的 file URL（或已是本地路径的字符串）转为本地路径。
    // QML 的全局 Qt 对象**没有** urlToLocalFile（那是 QUrl 的 C++ API），而用
    // 正则剔 file:// 前缀对 UNC（file://server/share/x）与含 %/#/? 的路径会给出
    // 错误结果，保存时甚至会静默写到带 percent 转义的乱码文件名里。
    // 无法转为本地路径（空值、非 file 协议）时返回空串
    Q_INVOKABLE QString toLocalPath(const QVariant &urlOrPath) const;

    // 供**渲染线程**调用（`QQuickImageProvider::requestImage` 不在 GUI 线程执行）：
    // 返回解密后的完整明文字节，仅用于应用内图片查看。明文只存在于返回值与
    // QImage 里，不落盘。超过 MaxInMemoryDecodeBytes 直接返回空：把整个明文
    // 读进内存只对小文件可行，大文件应走 saveToFile（流式解密写盘）
    QByteArray decryptedFileBytes(qint64 messageId) const;
    static constexpr qint64 MaxInMemoryDecodeBytes = 64 * 1024 * 1024;

    // M8.3b: 供 DecryptingIODevice（播放器）调用：一次性获取某消息的清单副本
    //（含密钥）与密文缓存文件路径。与 decryptedFileBytes 不同，本方法不读文件、
    // 不解密，只返回元信息，调用方自行流式逐片解密（明文不落盘）。
    // 返回 false 表示未登记清单、messageId 非法或缓存未就绪
    bool playbackInfoForMessage(qint64 messageId, XYChat::Protocol::FileManifest *manifestOut,
                                QString *cachePathOut) const;

    // M8.3a 欠账补齐：历史图片消息（M8.3a 之前发送，清单 thumb 为空）的本地
    // 缩略图生成。若清单是图片、thumb 为空、原图密文缓存已就绪且未超内存
    // 解码上限，则解密原图生成缩略图并缓存到 <cacheRoot>/thumbs/，返回 JPEG
    // 字节；已缓存则直接读取返回。非图片/原图未就绪/超限/生成失败返回空。
    // 供 NetworkManager::attachFileInfo 在清单 thumb 为空时回填 fileThumb
    QByteArray localThumbnailForMessage(qint64 messageId);

public slots:
    // 上传本地文件并在完成后请求发送。返回任务 token（参数非法也返回 token，
    // 便于 UI 统一挂进度并收到 taskFailed）。路径参数接受 QML 的 url 或本地
    // 路径字符串，内部经 toLocalPath 统一转换
    QString uploadAndSend(const QVariant &localPathOrUrl, qint64 conversationId,
                          qint64 peerUserId);
    // 取消任务：上传中会一并请求控制面取消（回收服务端已收分片）
    void cancelTask(const QString &token);
    // 下载收到的文件消息（清单须已由 registerIncomingFile 登记）
    QString download(qint64 messageId);
    // P4.3: 把已缓存的文件解密保存到用户指定路径（明文只在此处落盘）。改为异步：
    // 返回任务 token，解密写盘经时间片增量泵送，进度/结果经 taskProgress/
    // taskFinished/taskFailed 与 fileSaved 反馈（大文件不再冻结 GUI）。参数非法
    // 也返回 token 并同步发 taskFailed，便于 UI 统一挂进度与收失败
    QString saveToFile(qint64 messageId, const QVariant &destPathOrUrl);
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

    // 下载票据**连续**重新申请的次数上限。服务端下载票据按"最后一次使用"滑动续期，
    // 但存在绝对寿命上限（DownloadTicketMaxLifetimeSeconds），且空闲超窗口即失效；
    // 客户端收到 401 时凭此重新申请票据并从断点续传。必须封顶：否则"申请票据-再收到
    // 401"会变成活锁，与重试预算被打回同一类缺陷。
    // 注意是"连续"而非"累计"——任一分片成功落盘即清零（见 onGetFinished 成功分支），
    // 否则一台休眠过几次的机器下载大文件时会因累计满 3 次而永久失败并丢掉全部进度
    static constexpr int MaxTicketRenewals = 3;

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
        // M8.3b: 音视频时长（毫秒，异步提取，提取失败留空）
        qint64 mediaDurationMs = 0;
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
        // 已重新申请票据的次数（401 -> 重申请 -> 断点续传），受 MaxTicketRenewals 封顶
        int ticketRenewals = 0;

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

    // P4.4/P4.3: 本地 CPU/磁盘密集工作（上传前 hashing、另存为解密写盘）的时间片
    // 增量泵送。全程在 GUI 线程（无工作线程 → 无数据竞争 → 密文逐字节不变），
    // QTimer(0) 每轮事件循环推进一个受时间预算约束的批次后交还事件循环，从而
    // 大文件不再冻结界面。串行：一次只推进一个本地任务，其余排队
    void startLocalPump();
    void enqueueLocalWork(const QString &token);
    void advanceLocalWork();
    void runLocalSlice();
    void runHashSlice(Task &task);
    void runSaveSlice(Task &task);
    // 释放某任务的本地中间态（关闭并删除 HashState/SaveState、抹零保存密钥、
    // 复位 m_localToken 并推进队列）。cancelTask/finishTask/failTask/reset 均调用
    void clearLocalStateForToken(const QString &token);
    void pumpUpload(Task &task);
    void pumpDownload(Task &task);
    // 串行调度：一次只跑一个分片（避免带宽争抢、内存峰值与 per-IP 限流），
    // 当前分片完成后推进下一个可运行的任务
    void pumpNext();
    // M8.3b: 上传入口拆为"元数据提取（图片同步/音视频异步）→ hashing → creating"
    // 三阶段。beginHashing 从 uploadAndSend 抽出，供音视频元数据提取完成后回调
    void beginHashing(const QString &token);
    void startMetadataExtraction(const QString &token);
    void onMetadataExtracted(const MediaMetadataExtractor::Result &result);
    void processNextPendingExtraction();
    // M8.3b: 清理某任务的元数据提取状态（正在提取则取消并处理下一个，
    // 在队列中则移除）。cancelTask/finishTask/failTask 均调用，避免悬空 token
    void clearExtractionStateForToken(const QString &token);
    void onPutFinished(Task &task, QNetworkReply *reply);
    void onGetFinished(Task &task, QNetworkReply *reply);
    // 下载收尾：校验整体摘要后原子改名进缓存（不通过则丢弃临时文件）
    bool finalizeDownload(Task &task, QString *error);
    QNetworkRequest makeRequest(const QUrl &url) const;

    QString cachePathFor(const QString &sha256Hex) const;
    // M8.3a 欠账补齐：本地生成的缩略图缓存路径 <cacheRoot>/thumbs/<sha256[0..1]>/<sha256>.jpg
    //（与密文缓存分开：密文无后缀、缩略图是 JPEG 明文但可公开读取，不含密钥）
    QString thumbnailCachePathFor(const QString &sha256Hex) const;
    bool ensureCacheRoot();
    // 清单表的线程安全读取：渲染线程会经 decryptedFileBytes 访问，因此所有
    // 读取都走这里（加锁拷贝后立即解锁，文件 IO 与解密均在锁外做）。
    // 发信号也必须在锁外：接收方可能回调本类，否则会死锁
    bool manifestFor(qint64 messageId, XYChat::Protocol::FileManifest *out) const;
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
    // 保护 m_incoming：主线程写入（登记/reset），渲染线程读取（图片解码）
    mutable QMutex m_manifestMutex;
    qint64 m_nextSeqValue = 1;

    QString m_baseUrl;
    QString m_cacheRoot;
    QSslConfiguration m_sslConfig;

    // M8.3b: 音视频元数据异步提取器（懒创建，GUI 线程）。同一时间只提取
    // 一个文件，其余音视频上传任务排队等待（m_pendingExtractTokens）
    MediaMetadataExtractor *m_metadataExtractor = nullptr;
    QString m_extractingToken;
    QQueue<QString> m_pendingExtractTokens;

    // M8.3b: 历史图片缩略图本地生成的负缓存（按密文 sha256）。生成失败
    //（内容不可解码/压不进上限）时记下，避免每次 attachFileInfo 都重复
    // 全量解密同一份原图（无负缓存会退化为每次滚动/刷新都冻一下）。
    // 仅 GUI 线程访问（attachFileInfo），clearCache/reset 时清空以允许重试
    QSet<QString> m_thumbnailGenFailed;

    // P4.4/P4.3: 本地工作时间片泵（见上方方法注释）。QTimer(0) 惰性创建，
    // 无本地工作时停泵以免空转
    QTimer *m_localPump = nullptr;
    QString m_localToken;                 // 当前正在做本地工作的任务
    QQueue<QString> m_pendingLocalTokens; // 等待做本地工作的任务
    QElapsedTimer m_sliceTimer;           // 单个时间片的耗时预算计时

    // hashing/save 的中间态含不可拷贝的 QFile，故堆分配、按 token 索引；
    // 结构体定义在 .cpp（此处仅前向声明，避免把 FileCrypto 细节带进头文件）
    struct HashState;
    struct SaveState;
    QHash<QString, HashState *> m_hashStates;
    QHash<QString, SaveState *> m_saveStates;
};

} // namespace XYChat::Client
