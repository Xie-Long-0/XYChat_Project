#include "FileTransferManager.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QMimeDatabase>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>

#include "FileCrypto.h"
#include "SecureMemory.h"
#include "ThumbnailMaker.h"

namespace XYChat::Client
{

namespace
{
using XYChat::Protocol::FileManifest;
using XYChat::Security::FileCrypto;
using XYChat::Security::SecureMemory;
namespace Protocol = XYChat::Protocol;

const char TicketHeader[] = "X-XYChat-Ticket";

// 单个分片的网络重试上限。超过即判定为确定性故障并上报，不再无限重试
constexpr int MaxChunkAttempts = 3;
// 整个任务的"失败 -> 查询已收分片"恢复轮次上限。与分片重试分开计：
// 数据面持续 5xx 而控制面正常时，每次查询都成功，若仅靠分片重试计数
// 就会被不断重置而形成活锁
constexpr int MaxRecoveryRounds = 5;
// GCM 标签长度：明文分片为 0 时密文仍有这么长，故"解出空明文"只在
// 密文分片恰好等于标签长度时才是合法结果
constexpr qint64 GcmTagSize = 16;
// P4.4/P4.3: 本地工作单个时间片的耗时预算（毫秒）。每片至少处理一个分片，
// 随后在预算内继续，超预算即交还事件循环，兼顾进度与界面响应性
constexpr qint64 LocalSliceBudgetMs = 8;

// 网络类故障（可重试）与协议类故障（重试无益）必须分开：把 4xx 当瞬时错误
// 重试会白白消耗服务端 per-IP 失败配额，甚至触发限流。
// 取 NetworkError 枚举而不取 reply 指针：调用时 reply 已 deleteLater
bool isTransientFailure(QNetworkReply::NetworkError networkError, int status)
{
    if (status >= 500 && status <= 599) {
        return true;
    }
    if (status >= 400 && status < 500) {
        return false;
    }
    switch (networkError) {
    case QNetworkReply::NoError:
        return false;
    case QNetworkReply::ConnectionRefusedError:
    case QNetworkReply::RemoteHostClosedError:
    case QNetworkReply::TimeoutError:
    case QNetworkReply::TemporaryNetworkFailureError:
    case QNetworkReply::NetworkSessionFailedError:
    case QNetworkReply::ProxyTimeoutError:
    case QNetworkReply::UnknownNetworkError:
    case QNetworkReply::ProxyConnectionRefusedError:
    case QNetworkReply::ProxyConnectionClosedError:
        return true;
    default:
        return false;
    }
}
} // namespace

FileTransferManager::FileTransferManager(QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{
    m_cacheRoot = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QLatin1String("/filecache");
}

void FileTransferManager::setCacheRoot(const QString &path)
{
    if (!path.isEmpty()) {
        m_cacheRoot = path;
    }
}

QString FileTransferManager::cacheRoot() const
{
    return m_cacheRoot;
}

void FileTransferManager::setBaseUrl(const QString &url)
{
    if (m_baseUrl == url) {
        return;
    }
    m_baseUrl = url;
    emit baseUrlChanged();
}

QString FileTransferManager::baseUrl() const
{
    return m_baseUrl;
}

bool FileTransferManager::isEnabled() const
{
    return !m_baseUrl.isEmpty();
}

void FileTransferManager::setSslConfiguration(const QSslConfiguration &config)
{
    m_sslConfig = config;
}

// P4.4: hashing 中间态（QFile 不可拷贝，故堆分配、按 token 索引）。
// 第一遍只算摘要、不落临时文件：加密是确定性的（nonce 由 iv 与序号派生），
// 第二遍上传时重算得到的密文与这里逐字节相同
struct FileTransferManager::HashState
{
    QFile file;                 // 惰性打开的明文源（不在 beginHashing 同步占用句柄）
    FileCrypto::Sha256Stream digest;
    qint64 plainChunk = 0;      // 每片明文字节数（= 密文分片 - GCM 标签）
    qint64 offset = 0;          // 已处理的明文偏移
    int chunkCount = 0;         // 已加密的分片数
    qint64 cipherSize = 0;      // 累计密文字节
};

// P4.3: 另存为中间态（逐片解密写盘）
struct FileTransferManager::SaveState
{
    QFile src;                  // 密文缓存（惰性打开）
    QFile dst;                  // 目标临时文件（惰性打开）
    QString destPath;           // 用户最终目标路径
    QString tmpPath;            // destPath + ".xysaving"
    QString cachePath;          // 密文缓存路径
    qint64 offset = 0;          // 已处理的密文偏移
    qint64 written = 0;         // 已写出的明文字节
    int index = 0;              // 分片序号（解密用）
    FileManifest manifest;      // 含密钥/iv/分片口径
};

int FileTransferManager::activeTaskCount() const
{
    return static_cast<int>(m_tasks.size());
}

// P4.1: 活动任务快照。仅回脱敏展示字段（token/文件名/相位/方向/messageId），
// 绝不涵盖清单与密钥。进度比值由 QML 侧根据 taskProgress 的 done/total 维护
QVariantList FileTransferManager::tasks() const
{
    QVariantList out;
    out.reserve(m_tasks.size());
    for (auto it = m_tasks.constBegin(); it != m_tasks.constEnd(); ++it) {
        const Task &t = it.value();
        QVariantMap m;
        m.insert(QLatin1String("token"), t.token);
        m.insert(QLatin1String("fileName"), t.fileName);
        m.insert(QLatin1String("phase"), t.phase);
        m.insert(QLatin1String("isUpload"), t.isUpload);
        m.insert(QLatin1String("messageId"), t.messageId);
        out.append(m);
    }
    return out;
}

qint64 FileTransferManager::nextSeq()
{
    return m_nextSeqValue++;
}

qint64 FileTransferManager::requestSeq(const QString &token)
{
    const qint64 seq = nextSeq();
    m_seqToToken.insert(seq, token);
    return seq;
}

QString FileTransferManager::makeToken()
{
    return QUuid::createUuid().toString(QUuid::Id128);
}

FileTransferManager::Task *FileTransferManager::taskBySeq(qint64 seq)
{
    const auto it = m_seqToToken.constFind(seq);
    if (it == m_seqToToken.constEnd()) {
        return nullptr;
    }
    const QString token = it.value();
    m_seqToToken.remove(seq);
    const auto taskIt = m_tasks.find(token);
    return taskIt == m_tasks.end() ? nullptr : &taskIt.value();
}

QString FileTransferManager::cachePathFor(const QString &sha256Hex) const
{
    // 密文摘要作缓存键：内容寻址、不可预测、且能兼作完整性校验值。
    // 前两位十六进制作分桶，避免单目录项数随缓存增长（与服务端同款布局）
    if (!Protocol::isSha256Hex(sha256Hex)) {
        return QString();
    }
    return m_cacheRoot + QLatin1String("/") + sha256Hex.left(2) + QLatin1String("/") + sha256Hex;
}

QString FileTransferManager::thumbnailCachePathFor(const QString &sha256Hex) const
{
    // 与密文缓存分目录：密文无后缀（不可预测文件名兼作内容寻址），
    // 缩略图是 JPEG 明文（不含密钥，可公开读取）故带 .jpg 后缀
    if (!Protocol::isSha256Hex(sha256Hex)) {
        return QString();
    }
    return m_cacheRoot + QLatin1String("/thumbs/") + sha256Hex.left(2)
           + QLatin1String("/") + sha256Hex + QLatin1String(".jpg");
}

bool FileTransferManager::ensureCacheRoot()
{
    QDir dir(m_cacheRoot);
    return dir.exists() || QDir().mkpath(m_cacheRoot);
}

bool FileTransferManager::isCached(const QString &sha256Hex, qint64 cipherSize) const
{
    const QString path = cachePathFor(sha256Hex);
    if (path.isEmpty()) {
        return false;
    }
    const QFileInfo info(path);
    // 只信"存在且字节数相符"：内容是否真的完好交给解密时的 GCM 认证，
    // 每次命中都重算整体摘要会让缓存命中失去意义
    return info.exists() && info.isFile() && info.size() == cipherSize;
}

bool FileTransferManager::isMessageFileAvailable(qint64 messageId) const
{
    FileManifest manifest;
    if (!manifestFor(messageId, &manifest)) {
        return false;
    }
    return isCached(manifest.sha256Hex, manifest.cipherSize);
}

QString FileTransferManager::toLocalPath(const QVariant &urlOrPath) const
{
    // QML 的 url 类型经 QVariant 传来时保留为 QUrl，直接取 toLocalFile 最准确：
    // 经字符串中转会引入 percent-encoding 的二次编解码歧义（QUrl::toString 默认
    // PrettyDecoded，对 % 会转成 %25）
    if (urlOrPath.typeId() == QMetaType::QUrl) {
        const QUrl url = urlOrPath.toUrl();
        return url.isLocalFile() ? url.toLocalFile() : QString();
    }
    const QString text = urlOrPath.toString().trimmed();
    if (text.isEmpty()) {
        return QString();
    }
    // 字符串形态：以 "scheme:" 开头即当 URL 解析，只有 file: 协议才接受。
    // 不把非 file 协议的字符串当路径原样返回：否则 "https://x/y" 会被
    // QFileInfo 当作相对路径拿去 open，错误现场更难定位（用户看到的是
    // "文件不存在"，而不是 "这不是本地路径"）。
    // 但需让开 Windows 盘符路径（C:/... 与 C:\...）：冒号前只有一个字母时
    // 是盘符而不是 URL scheme（URL scheme 至少 2 个字符）
    const int colon = text.indexOf(QLatin1Char(':'));
    const bool looksLikeDriveLetter = colon == 1 && text.at(0).isLetter();
    if (colon > 0 && !looksLikeDriveLetter && colon < text.indexOf(QLatin1Char('/'))) {
        const QUrl url(text);
        return url.isLocalFile() ? url.toLocalFile() : QString();
    }
    // 已是本地路径（包括 UNC 的 \\server\share 形式、相对路径与 Windows 盘符路径）
    return text;
}

bool FileTransferManager::manifestFor(qint64 messageId,
                                      XYChat::Protocol::FileManifest *out) const
{
    if (out == nullptr || messageId <= 0) {
        return false;
    }
    // 加锁只为了拷贝一份清单：文件 IO 与解密均在锁外做，避免渲染线程
    // 长时间持锁堵住主线程的登记/reset
    QMutexLocker locker(&m_manifestMutex);
    const auto it = m_incoming.constFind(messageId);
    if (it == m_incoming.constEnd() || !it->isValid()) {
        return false;
    }
    *out = it.value();
    return true;
}

QByteArray FileTransferManager::decryptedFileBytes(qint64 messageId) const
{
    FileManifest manifest;
    if (!manifestFor(messageId, &manifest)) {
        return {};
    }
    // 只服务应用内图片查看：把整个明文读进内存仅对小文件可行，大文件应走
    // saveToFile（流式解密写盘），否则一个 2 GiB 附件就能把进程打爆
    if (manifest.plainSize <= 0 || manifest.plainSize > MaxInMemoryDecodeBytes) {
        SecureMemory::wipe(manifest.key);
        return {};
    }

    QFile file(cachePathFor(manifest.sha256Hex));
    if (!file.open(QIODevice::ReadOnly)) {
        SecureMemory::wipe(manifest.key);
        return {};  // 未下载或缓存已清：UI 应回退到“需下载”状态
    }

    QByteArray plain;
    plain.reserve(manifest.plainSize);
    qint64 offset = 0;
    int index = 0;
    bool corrupt = false;
    while (offset < manifest.cipherSize) {
        const qint64 want = qMin(manifest.chunkSize, manifest.cipherSize - offset);
        const QByteArray cipher = file.read(want);
        if (cipher.size() != want) {
            corrupt = true;
            break;
        }
        const QByteArray chunk =
            FileCrypto::decryptChunk(manifest.key, manifest.iv, index, cipher);
        // 密文分片恰好只有一个标签长时，明文为空是合法结果；其余情况下
        // 空返回值意味着 GCM 认证失败（缓存损坏或密钥不符）
        if (chunk.isEmpty() && want > GcmTagSize) {
            corrupt = true;
            break;
        }
        plain += chunk;
        offset += want;
        ++index;
    }
    file.close();
    SecureMemory::wipe(manifest.key);

    // 长度不符或 GCM 认证失败都归为损坏：宁可返回空让 UI 提示重新下载，
    // 也不把半截数据当图片解码
    if (corrupt || plain.size() != manifest.plainSize) {
        plain.clear();
    }
    return plain;
}

bool FileTransferManager::playbackInfoForMessage(qint64 messageId,
                                                 XYChat::Protocol::FileManifest *manifestOut,
                                                 QString *cachePathOut) const
{
    if (manifestOut == nullptr || cachePathOut == nullptr) {
        return false;
    }
    if (!manifestFor(messageId, manifestOut)) {
        return false;
    }
    // 缓存未就绪（未下载或已被清理）：播放器无法工作，调用方应先触发 download
    if (!isCached(manifestOut->sha256Hex, manifestOut->cipherSize)) {
        SecureMemory::wipe(manifestOut->key);
        return false;
    }
    *cachePathOut = cachePathFor(manifestOut->sha256Hex);
    if (cachePathOut->isEmpty()) {
        SecureMemory::wipe(manifestOut->key);
        return false;
    }
    return true;
}

QByteArray FileTransferManager::localThumbnailForMessage(qint64 messageId)
{
    FileManifest manifest;
    if (!manifestFor(messageId, &manifest)) {
        return {};
    }
    // 只补齐图片：音视频封面走清单 thumb（M8.3b 上传时生成），非图片无缩略图。
    // 清单已带 thumb（M8.3a 之后发送的图片）也无需本地补齐
    if (!manifest.mime.startsWith(QLatin1String("image/")) || !manifest.thumbnail.isEmpty()) {
        SecureMemory::wipe(manifest.key);
        return {};
    }

    const QString thumbPath = thumbnailCachePathFor(manifest.sha256Hex);
    if (thumbPath.isEmpty()) {
        SecureMemory::wipe(manifest.key);
        return {};
    }
    // 负缓存命中：此前已尝试生成但失败（内容不可解码/压不进上限），不再重复
    // 全量解密原图（否则每次滚动/刷新都会冻一下）
    if (m_thumbnailGenFailed.contains(manifest.sha256Hex)) {
        SecureMemory::wipe(manifest.key);
        return {};
    }
    // 缩略图缓存命中：直接读取，避免重复解密原图
    {
        QFile cached(thumbPath);
        if (cached.exists() && cached.open(QIODevice::ReadOnly)) {
            const QByteArray data = cached.readAll();
            cached.close();
            SecureMemory::wipe(manifest.key);
            if (!data.isEmpty()) {
                return data;
            }
            return {};
        }
    }
    // 原图密文缓存未就绪：无法生成（UI 应先触发 download）
    if (!isCached(manifest.sha256Hex, manifest.cipherSize)) {
        SecureMemory::wipe(manifest.key);
        return {};
    }
    // 解密原图（内存）生成缩略图。decryptedFileBytes 有 64 MiB 上限，
    // 超限的大图不补齐（UI 回退到文件图标），避免把进程打爆
    const QByteArray plain = decryptedFileBytes(messageId);
    SecureMemory::wipe(manifest.key);
    if (plain.isEmpty()) {
        return {};
    }
    const QImage image = QImage::fromData(plain);
    if (image.isNull()) {
        // 负缓存：内容不可解码（清单 mime 标称 image/* 但实际不是），避免每次
        // 渲染都重复全量解密。sha256Hex 不受上面 key 的 wipe 影响
        m_thumbnailGenFailed.insert(manifest.sha256Hex);
        return {};
    }
    const QByteArray thumb = ThumbnailMaker::encodeThumbnail(image);
    if (thumb.isEmpty()) {
        m_thumbnailGenFailed.insert(manifest.sha256Hex);  // 负缓存：压不进上限
        return {};
    }
    // 写入缩略图缓存（临时名 + 改名，避免半截文件被当作完整缓存）。
    // 写失败不影响本次返回（下次会重试生成）
    QDir().mkpath(QFileInfo(thumbPath).absolutePath());
    const QString tmpPath = thumbPath + QLatin1String(".")
        + QUuid::createUuid().toString(QUuid::Id128) + QLatin1String(".tmp");
    QFile tmp(tmpPath);
    if (tmp.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (tmp.write(thumb) == thumb.size()) {
            tmp.close();
            if (QFileInfo::exists(thumbPath)) {
                QFile::remove(thumbPath);
            }
            if (!tmp.rename(thumbPath)) {
                tmp.remove();
            }
        } else {
            tmp.close();
            tmp.remove();
        }
    }
    return thumb;
}

qint64 FileTransferManager::cacheBytes() const
{
    qint64 total = 0;
    QDir root(m_cacheRoot);
    const QStringList buckets = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &bucket : buckets) {
        QDir dir(root.filePath(bucket));
        const QFileInfoList files = dir.entryInfoList(QDir::Files | QDir::NoSymLinks);
        for (const QFileInfo &info : files) {
            total += info.size();
        }
    }
    return total;
}

int FileTransferManager::clearCache()
{
    int removed = 0;
    QDir root(m_cacheRoot);
    // M8.3a 欠账补齐：先递归清理本地缩略图缓存（thumbs 目录）。缩略图是
    // 密文的衍生副本，清掉后下次需要时会从原图重新生成
    const QString thumbsDir = m_cacheRoot + QLatin1String("/thumbs");
    if (QDir(thumbsDir).exists()) {
        QDir thumbsRoot(thumbsDir);
        const QStringList thumbBuckets =
            thumbsRoot.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &bucket : thumbBuckets) {
            QDir dir(thumbsRoot.filePath(bucket));
            removed += dir.entryList(QDir::Files | QDir::NoSymLinks).count();
        }
        // removeRecursively 是无参成员函数：删除 QDir 对象代表的目录及其全部内容
        QDir(thumbsDir).removeRecursively();
    }
    // 缩略图负缓存一并清空：缓存已清，重新下载后应允许重试生成
    m_thumbnailGenFailed.clear();
    const QStringList buckets = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &bucket : buckets) {
        // thumbs 已单独递归清理（且已删除），此处跳过以防万一
        if (bucket == QLatin1String("thumbs")) {
            continue;
        }
        QDir dir(root.filePath(bucket));
        const QStringList files = dir.entryList(QDir::Files | QDir::NoSymLinks);
        for (const QString &name : files) {
            if (dir.remove(name)) {
                ++removed;
            }
        }
        // 空桶目录一并移除，避免长期运行后留下大量空目录
        if (dir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty()) {
            root.rmdir(bucket);
        }
    }
    // 缓存已清空，所有已登记的文件消息回到"缺失"态（可重新下载）。
    // 先加锁拷贝键再在锁外发信号：接收方可能回调本类，持锁发信号会死锁
    QList<qint64> registered;
    {
        QMutexLocker locker(&m_manifestMutex);
        registered = m_incoming.keys();
    }
    for (qint64 messageId : registered) {
        emit downloadStateChanged(messageId, QLatin1String("missing"));
    }
    emit cacheCleared(removed);
    return removed;
}

void FileTransferManager::registerIncomingFile(qint64 messageId, const QString &manifestJson)
{
    if (messageId <= 0) {
        return;
    }
    bool ok = false;
    // fail-closed：清单非法（版本不符/字段缺失/口径不自洽）时不登记，
    // 后续下载与保存都会因查不到清单而明确失败，不渲染半截元数据
    const FileManifest manifest = Protocol::decodeFileManifest(manifestJson, &ok);
    if (!ok || !manifest.isValid()) {
        return;
    }
    const bool already = m_incoming.contains(messageId);
    Q_UNUSED(already);
    {
        QMutexLocker locker(&m_manifestMutex);
        m_incoming.insert(messageId, manifest);
    }
    // 发信号在锁外（接收方可能回调本类）
    emit downloadStateChanged(messageId,
                              isCached(manifest.sha256Hex, manifest.cipherSize)
                                  ? QLatin1String("available")
                                  : QLatin1String("missing"));
}

QString FileTransferManager::uploadAndSend(const QVariant &localPathOrUrl,
                                           qint64 conversationId, qint64 peerUserId)
{
    // QML 的 FileDialog 给的是 file URL，而引擎需要本地路径：统一在此转换，
    // 不把转换责任下放给 QML（QML 侧无可靠的 url → 本地路径手段）
    const QString localPath = toLocalPath(localPathOrUrl);

    Task task;
    task.token = makeToken();
    task.isUpload = true;
    task.localPath = localPath;
    task.conversationId = conversationId;
    task.peerUserId = peerUserId;
    task.chunkSize = Protocol::DefaultChunkSize;
    const QString token = task.token;
    m_tasks.insert(token, task);
    emit tasksChanged();

    if (!isEnabled()) {
        failTask(token, QLatin1String("File transfer is not available on this server"));
        return token;
    }
    if (localPath.isEmpty()) {
        failTask(token, QLatin1String("Not a local file path"));
        return token;
    }
    const QFileInfo info(localPath);
    if (!info.exists() || !info.isFile()) {
        failTask(token, QLatin1String("File does not exist"));
        return token;
    }
    if (!info.isReadable()) {
        failTask(token, QLatin1String("File is not readable"));
        return token;
    }
    if (info.size() <= 0) {
        failTask(token, QLatin1String("File is empty"));
        return token;
    }

    Task &t = m_tasks[token];
    t.plainSize = info.size();
    t.fileName = info.fileName();
    // 文件名长度按协议上限校验：清单 fail-closed，超限会被拒编码
    if (t.fileName.size() > Protocol::MaxFileNameLength) {
        t.fileName = t.fileName.right(Protocol::MaxFileNameLength);
    }
    t.mime = QMimeDatabase().mimeTypeForFile(localPath).name();
    // M8.3: 图片尺寸与内联缩略图（纯 QtGui 同步提取，无平台多媒体后端依赖）。
    // **先按 MIME 过滤再探测**：对任意二进制文件跑图像格式探测既浪费，也会
    // 让解码器读到垃圾数据（随机字节可能偶然命中某格式的魔数）。
    // 非图片或提取失败时字段留空，UI 回退到文件图标：元数据缺失绝不阻断发送
    if (t.mime.startsWith(QLatin1String("image/"))) {
        const ThumbnailMaker::Result media = ThumbnailMaker::make(localPath);
        if (media.isImage) {
            t.mediaWidth = media.width;
            t.mediaHeight = media.height;
            t.thumbnail = media.thumb;
        }
        // 图片同步提取完成，直接开始 hashing
        beginHashing(token);
        return token;
    }

    // M8.3b: 音视频元数据异步提取（QMediaPlayer + QVideoSink，依赖平台解码后端）。
    // 提取期间 phase="extracting"，UI 显示"提取元数据中 0%"；提取完成或超时后
    // 继续 hashing。元数据缺失绝不阻断发送（与图片路径同口径）。同一时间只
    // 提取一个文件，其余音视频上传任务排队等待
    if (t.mime.startsWith(QLatin1String("audio/"))
        || t.mime.startsWith(QLatin1String("video/"))) {
        t.phase = QLatin1String("extracting");
        emit taskProgress(token, t.phase, 0, t.plainSize);
        if (m_extractingToken.isEmpty()) {
            startMetadataExtraction(token);
        } else {
            m_pendingExtractTokens.enqueue(token);
        }
        return token;
    }

    // 其他文件（非图片非音视频）：无元数据可提取，直接开始 hashing
    beginHashing(token);
    return token;
}

// M8.3b/P4.4: hashing → creating 阶段（从 uploadAndSend 抽出，供音视频元数据
// 提取完成后回调）。P4.4: hashing 不再在此同步跑完（大文件会冻屏），改为
// 生成密钥、建 HashState 后入本地泵队列，由 QTimer(0) 时间片增量推进；文件
// 句柄在首个切片惰性打开（避免同步占用句柄，也保证源文件在入队后被删时能
// 在泵送时明确失败）
void FileTransferManager::beginHashing(const QString &token)
{
    auto it = m_tasks.find(token);
    if (it == m_tasks.end() || it->cancelled) {
        return;
    }
    Task &t = it.value();
    t.phase = QLatin1String("hashing");

    // 生成文件密钥（廉价，同步做）；每个文件独立密钥，严禁跨文件复用
    const FileCrypto::FileKey key = FileCrypto::generateFileKey();
    if (!key.valid) {
        failTask(token, QLatin1String("Failed to generate a file key"));
        return;
    }
    t.fileKey = key.key;
    t.iv = key.iv;
    // 明文分片 = 密文分片 - GCM 标签，使每片密文恰好等于 chunkSize
    const qint64 plainChunk = Protocol::plainSizeOfChunk(t.chunkSize);
    if (plainChunk <= 0) {
        failTask(token, QLatin1String("Invalid chunk size"));
        return;
    }

    auto *st = new HashState;
    st->plainChunk = plainChunk;
    m_hashStates.insert(token, st);
    // 首片 taskProgress（hashing 0%）：横幅立即有反馈；后续进度由 runHashSlice 发
    emit taskProgress(token, t.phase, 0, t.plainSize);
    enqueueLocalWork(token);
}

// M8.3b: 开始音视频元数据提取（懒创建 extractor，连接 finished 信号）
void FileTransferManager::startMetadataExtraction(const QString &token)
{
    auto it = m_tasks.find(token);
    if (it == m_tasks.end() || it->cancelled) {
        // 任务已被取消/失败，跳过并处理下一个
        m_extractingToken.clear();
        processNextPendingExtraction();
        return;
    }
    if (!m_metadataExtractor) {
        m_metadataExtractor = new MediaMetadataExtractor(this);
        connect(m_metadataExtractor, &MediaMetadataExtractor::finished, this,
                &FileTransferManager::onMetadataExtracted);
    }
    m_extractingToken = token;
    m_metadataExtractor->extract(it->localPath);
}

// M8.3b: 元数据提取完成回调。提取失败或超时则元数据留空，继续 hashing
//（绝不阻断发送）。然后处理队列中下一个等待提取的任务
void FileTransferManager::onMetadataExtracted(const MediaMetadataExtractor::Result &result)
{
    const QString token = m_extractingToken;
    m_extractingToken.clear();

    auto it = m_tasks.find(token);
    if (it != m_tasks.end() && !it->cancelled) {
        Task &t = it.value();
        if (result.ok) {
            t.mediaDurationMs = result.durationMs;
            t.mediaWidth = result.width;
            t.mediaHeight = result.height;
            t.thumbnail = result.thumbnail;
        }
        beginHashing(token);
    }

    processNextPendingExtraction();
}

// M8.3b: 从队列取出下一个待提取的任务（跳过已取消/失败的）
void FileTransferManager::processNextPendingExtraction()
{
    while (!m_pendingExtractTokens.isEmpty()) {
        const QString nextToken = m_pendingExtractTokens.dequeue();
        if (m_tasks.contains(nextToken) && !m_tasks[nextToken].cancelled) {
            startMetadataExtraction(nextToken);
            return;
        }
    }
}

void FileTransferManager::clearExtractionStateForToken(const QString &token)
{
    if (m_extractingToken == token) {
        if (m_metadataExtractor) {
            m_metadataExtractor->cancel();
        }
        m_extractingToken.clear();
        processNextPendingExtraction();
    } else {
        m_pendingExtractTokens.removeAll(token);
    }
}

// P4.4/P4.3: 惰性创建并启动本地工作泵（QTimer(0)）。每轮事件循环触发一次
// runLocalSlice，处理一个时间片后交还事件循环，界面因此保持响应
void FileTransferManager::startLocalPump()
{
    if (m_resetting) {
        return;
    }
    if (!m_localPump) {
        m_localPump = new QTimer(this);
        m_localPump->setInterval(0);
        connect(m_localPump, &QTimer::timeout, this, &FileTransferManager::runLocalSlice);
    }
    if (!m_localPump->isActive()) {
        m_localPump->start();
    }
}

// P4.4/P4.3: 把一个待做本地工作的任务入队。串行：若当前无本地任务则
// 立即取它，否则排队等待；无论如何都确保泵在跑
void FileTransferManager::enqueueLocalWork(const QString &token)
{
    m_pendingLocalTokens.enqueue(token);
    if (m_localToken.isEmpty()) {
        advanceLocalWork();
    }
    startLocalPump();
}

// P4.4/P4.3: 从队列取下一个仍需本地工作的任务作为当前任务（跳过已消失/
// 已取消/无本地态的）。取不到则清空 m_localToken（泵随后自停）
void FileTransferManager::advanceLocalWork()
{
    m_localToken.clear();
    while (!m_pendingLocalTokens.isEmpty()) {
        const QString next = m_pendingLocalTokens.dequeue();
        const auto it = m_tasks.constFind(next);
        if (it == m_tasks.constEnd() || it->cancelled) {
            continue;
        }
        if (!m_hashStates.contains(next) && !m_saveStates.contains(next)) {
            continue;
        }
        m_localToken = next;
        return;
    }
}

// P4.4/P4.3: 泵回调——推进当前本地任务一个时间片。无可推进任务时停泵
void FileTransferManager::runLocalSlice()
{
    if (m_resetting) {
        return;
    }
    if (m_localToken.isEmpty() || !m_tasks.contains(m_localToken)) {
        advanceLocalWork();
    }
    if (m_localToken.isEmpty()) {
        if (m_localPump) {
            m_localPump->stop();
        }
        return;
    }
    const auto it = m_tasks.find(m_localToken);
    if (it == m_tasks.end()) {
        advanceLocalWork();
        return;
    }
    Task &task = it.value();
    if (task.cancelled) {
        const QString token = m_localToken;
        clearLocalStateForToken(token);
        finishTask(token);
    } else if (m_hashStates.contains(task.token)) {
        runHashSlice(task);
    } else if (m_saveStates.contains(task.token)) {
        runSaveSlice(task);
    } else {
        // 本地态已被清理（意外）：放弃并推进队列
        clearLocalStateForToken(task.token);
    }
    // 本片结束：若仍有本地工作，泵会在下一轮事件循环再次触发；否则停泵
    if (m_localToken.isEmpty() && m_pendingLocalTokens.isEmpty()) {
        if (m_localPump) {
            m_localPump->stop();
        }
    }
}

// P4.4: 推进 hashing 一个时间片。完成后校验分片口径并转入 creating（发
// uploadCreateRequested）；中途出错则清理并失败。文件句柄首片惰性打开
void FileTransferManager::runHashSlice(Task &task)
{
    HashState *st = m_hashStates.value(task.token);
    if (!st) {
        failTask(task.token, QLatin1String("Hashing state was lost"));
        return;
    }
    if (!st->file.isOpen()) {
        st->file.setFileName(task.localPath);
        if (!st->file.open(QIODevice::ReadOnly)) {
            const QString token = task.token;
            clearLocalStateForToken(token);
            failTask(token, QLatin1String("Cannot open file for reading"));
            return;
        }
    }

    m_sliceTimer.restart();
    QString error;
    bool done = false;
    // 至少处理一片，随后在时间预算内继续（do-while 保证进度，超预算即让出）。
    // 加密口径与同步版逐字一致：同密钥/iv/分片序号 → 同密文 → 同整体摘要
    do {
        const qint64 want = qMin(st->plainChunk, task.plainSize - st->offset);
        const QByteArray plain = st->file.read(want);
        if (plain.size() != want) {
            error = QLatin1String("Failed to read file");
            break;
        }
        const QByteArray cipher = FileCrypto::encryptChunk(task.fileKey, task.iv, st->chunkCount, plain);
        // 空明文分片也会产出标签，故成功时密文必不为空
        if (cipher.isEmpty()) {
            error = QLatin1String("Failed to encrypt file");
            break;
        }
        st->digest.addData(cipher);
        st->cipherSize += cipher.size();
        st->offset += want;
        ++st->chunkCount;
        emit taskProgress(task.token, QLatin1String("hashing"), st->offset, task.plainSize);
        if (st->offset >= task.plainSize) {
            done = true;
            break;
        }
    } while (m_sliceTimer.elapsed() < LocalSliceBudgetMs);

    if (!error.isEmpty()) {
        const QString token = task.token;
        clearLocalStateForToken(token);
        failTask(token, error);
        return;
    }
    if (!done) {
        return;  // 时间片用尽，泵的下一轮继续
    }

    // hashing 完成：校验分片口径（与同步版逐条一致）
    if (st->cipherSize > Protocol::MaxFileSize) {
        error = QLatin1String("File exceeds the maximum size");
    } else if (st->chunkCount > Protocol::MaxChunkCount) {
        error = QLatin1String("File has too many chunks");
    } else if (!Protocol::isChunkingValid(st->cipherSize, task.chunkSize, st->chunkCount)) {
        error = QLatin1String("Chunking parameters are inconsistent");
    }

    const QString token = task.token;
    const qint64 cipherSize = st->cipherSize;
    const int chunkCount = st->chunkCount;
    const QString sha256Hex = error.isEmpty() ? st->digest.hexDigest() : QString();
    // 先清理本地态（关闭句柄、复位 m_localToken 并推进队列），再转 creating 发信号：
    // emit 可能被控制面同步回调（onUploadCreated → pumpUpload），届时本任务已
    // 不再是本地工作，避免 m_localToken 悬空
    clearLocalStateForToken(token);
    if (!error.isEmpty()) {
        failTask(token, error);
        return;
    }
    const auto tit = m_tasks.find(token);
    if (tit == m_tasks.end() || tit->cancelled) {
        return;
    }
    Task &t = tit.value();
    t.cipherSize = cipherSize;
    t.chunkCount = chunkCount;
    t.sha256Hex = sha256Hex;
    t.phase = QLatin1String("creating");
    emit taskProgress(token, t.phase, 0, t.cipherSize);
    emit uploadCreateRequested(requestSeq(token), t.cipherSize, t.chunkSize, t.chunkCount,
                               t.sha256Hex);
}

// P4.3: 推进另存为一个时间片（逐片解密写目标临时文件）。完成后校验长度并
// 原子改名到目标、发 fileSaved + 结束任务；缓存损坏/写失败分开处理
void FileTransferManager::runSaveSlice(Task &task)
{
    SaveState *st = m_saveStates.value(task.token);
    if (!st) {
        failTask(task.token, QLatin1String("Save state was lost"));
        return;
    }
    if (!st->src.isOpen()) {
        st->src.setFileName(st->cachePath);
        if (!st->src.open(QIODevice::ReadOnly)) {
            const QString token = task.token;
            clearLocalStateForToken(token);
            failTask(token, QLatin1String("Cannot read the cached file"));
            return;
        }
    }
    if (!st->dst.isOpen()) {
        st->dst.setFileName(st->tmpPath);
        if (!st->dst.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            const QString token = task.token;
            const QString tmp = st->tmpPath;
            clearLocalStateForToken(token);
            QFile::remove(tmp);
            failTask(token, QLatin1String("Cannot write the destination file"));
            return;
        }
    }

    const FileManifest mf = st->manifest;
    m_sliceTimer.restart();
    bool cacheCorrupt = false;
    bool destFailed = false;
    bool done = false;
    do {
        const qint64 want = qMin(mf.chunkSize, mf.cipherSize - st->offset);
        const QByteArray cipher = st->src.read(want);
        if (cipher.size() != want) {
            cacheCorrupt = true;
            break;
        }
        const QByteArray plain = FileCrypto::decryptChunk(mf.key, mf.iv, st->index, cipher);
        // 密文分片恰好只有一个标签长时，明文为空是合法结果；其余情况下
        // 空返回值意味着 GCM 认证失败（缓存损坏或密钥不符）
        if (plain.isEmpty() && want > GcmTagSize) {
            cacheCorrupt = true;
            break;
        }
        if (st->dst.write(plain) != plain.size()) {
            destFailed = true;
            break;
        }
        st->written += plain.size();
        st->offset += want;
        ++st->index;
        emit taskProgress(task.token, QLatin1String("saving"), st->offset, mf.cipherSize);
        if (st->offset >= mf.cipherSize) {
            done = true;
            break;
        }
    } while (m_sliceTimer.elapsed() < LocalSliceBudgetMs);

    // 先把后续需要的值拷出局部，再清理本地态（clearLocalStateForToken 会删 SaveState）
    const QString token = task.token;
    const QString tmpPath = st->tmpPath;
    const QString destPath = st->destPath;
    const QString cachePath = st->cachePath;
    const qint64 messageId = task.messageId;
    const qint64 written = st->written;
    const qint64 plainSize = mf.plainSize;

    if (cacheCorrupt || destFailed) {
        // 两类失败分开：缓存损坏（读侧/GCM 失败）删缓存让用户重下；目标写
        // 失败（磁盘满/无权限）时缓存完好，删它只会迫使重下且大概率再次失败
        clearLocalStateForToken(token);
        QFile::remove(tmpPath);
        if (cacheCorrupt) {
            QFile::remove(cachePath);
            emit downloadStateChanged(messageId, QLatin1String("missing"));
        }
        failTask(token, destFailed ? QLatin1String("Failed to write the destination file")
                                   : QLatin1String("Cached file failed the integrity check"));
        return;
    }
    if (!done) {
        return;  // 时间片用尽，下一轮继续
    }

    // 完成：先关文件（clearLocalStateForToken），再校验长度并原子改名
    clearLocalStateForToken(token);
    if (written != plainSize) {
        QFile::remove(tmpPath);
        QFile::remove(cachePath);
        emit downloadStateChanged(messageId, QLatin1String("missing"));
        failTask(token, QLatin1String("Saved size does not match the manifest"));
        return;
    }
    if (QFileInfo::exists(destPath)) {
        QFile::remove(destPath);
    }
    if (!QFile::rename(tmpPath, destPath)) {
        QFile::remove(tmpPath);
        failTask(token, QLatin1String("Cannot move the saved file into place"));
        return;
    }
    emit fileSaved(messageId, destPath);
    finishTask(token);
}

// P4.4/P4.3: 释放某任务的本地中间态。关闭并删除 HashState/SaveState、抹零保存
// 密钥、复位 m_localToken 并推进队列。cancelTask/finishTask/failTask/reset 与
// 各切片完成/失败路径均调用，确保 QFile 句柄与明文密钥不残留
void FileTransferManager::clearLocalStateForToken(const QString &token)
{
    if (HashState *st = m_hashStates.take(token)) {
        if (st->file.isOpen()) {
            st->file.close();
        }
        delete st;
    }
    if (SaveState *sv = m_saveStates.take(token)) {
        if (sv->src.isOpen()) {
            sv->src.close();
        }
        if (sv->dst.isOpen()) {
            sv->dst.close();
        }
        // 保存密钥用后清零（manifest.key 是明文密钥的堆副本）
        SecureMemory::wipe(sv->manifest.key);
        delete sv;
    }
    if (m_localToken == token) {
        advanceLocalWork();
    } else {
        m_pendingLocalTokens.removeAll(token);
    }
}

void FileTransferManager::onUploadCreated(qint64 seq, bool ok, qint64 fileId,
                                          const QString &ticket, const QString &error)
{
    Task *task = taskBySeq(seq);
    if (!task) {
        return;  // 任务已取消或从未发起
    }
    if (!ok || fileId <= 0 || ticket.isEmpty()) {
        failTask(task->token, error.isEmpty() ? QLatin1String("Failed to create upload") : error);
        return;
    }
    task->fileId = fileId;
    task->uploadTicket = ticket;
    task->remainingChunks.clear();
    for (int i = 0; i < task->chunkCount; ++i) {
        task->remainingChunks.insert(i);
    }
    task->nextChunk = 0;
    task->bytesDone = 0;
    task->phase = QLatin1String("uploading");
    pumpNext();
}

void FileTransferManager::pumpNext()
{
    // 串行：一次只有一个在途分片，避免带宽争抢、内存峰值与服务端 per-IP 限流。
    // m_pumping 拦住嵌套：failTask/finishTask 会递归调本函数，而它们可能
    // 正是在本函数的循环体内被同步调起的
    if (m_activeReply || m_pumping || m_resetting) {
        return;
    }
    m_pumping = true;
    // 先取 token 快照再遍历：循环体内可能同步走到 failTask/finishTask，
    // 它们会 erase 任务节点，直接在 QHash 上迭代会使当前迭代器失效
    const QStringList tokens = m_tasks.keys();
    for (const QString &token : tokens) {
        if (m_activeReply || m_resetting) {
            break;
        }
        const auto it = m_tasks.find(token);
        if (it == m_tasks.end() || it->cancelled) {
            continue;
        }
        Task &task = it.value();
        if (task.isUpload) {
            if (task.fileId > 0 && !task.uploadTicket.isEmpty()
                && !task.remainingChunks.isEmpty()) {
                pumpUpload(task);
            }
        } else if (!task.downloadTicket.isEmpty() && !task.tmpPath.isEmpty()) {
            pumpDownload(task);
        }
    }
    m_pumping = false;
}

void FileTransferManager::pumpUpload(Task &task)
{
    if (task.cancelled || m_activeReply) {
        return;
    }
    // 取最小的待传分片：乱序上传没有意义，顺序推进也让进度可读
    int index = -1;
    for (int candidate : task.remainingChunks) {
        if (index < 0 || candidate < index) {
            index = candidate;
        }
    }
    if (index < 0) {
        // 全部分片已上传，宣告完成（服务端流式组装并核对整体摘要）
        task.phase = QLatin1String("completing");
        emit taskProgress(task.token, task.phase, task.cipherSize, task.cipherSize);
        emit uploadCompleteRequested(requestSeq(task.token), task.fileId);
        return;
    }

    QFile file(task.localPath);
    if (!file.open(QIODevice::ReadOnly)) {
        failTask(task.token, QLatin1String("Cannot open file for reading"));
        return;
    }
    const qint64 plainChunk = Protocol::plainSizeOfChunk(task.chunkSize);
    const qint64 offset = plainChunk * index;
    if (!file.seek(offset)) {
        failTask(task.token, QLatin1String("Failed to seek in file"));
        return;
    }
    const qint64 want = qMin(plainChunk, task.plainSize - offset);
    const QByteArray plain = file.read(want);
    file.close();
    if (plain.size() != want) {
        failTask(task.token, QLatin1String("Failed to read file"));
        return;
    }

    const QByteArray cipher = FileCrypto::encryptChunk(task.fileKey, task.iv, index, plain);
    if (cipher.isEmpty()) {
        failTask(task.token, QLatin1String("Failed to encrypt chunk"));
        return;
    }

    const QUrl url(m_baseUrl + QLatin1String("/") + QString::number(task.fileId)
                   + QLatin1String("/chunk/") + QString::number(index));
    QNetworkRequest request = makeRequest(url);
    request.setRawHeader(TicketHeader, task.uploadTicket.toLatin1());
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");

    task.activeIndex = index;
    task.phase = QLatin1String("uploading");
    m_activeToken = task.token;
    m_activeReply = m_nam->put(request, cipher);
    connect(m_activeReply, &QNetworkReply::finished, this, [this, reply = m_activeReply]() {
        Task *current = m_tasks.find(m_activeToken) == m_tasks.end()
            ? nullptr
            : &m_tasks[m_activeToken];
        if (current) {
            onPutFinished(*current, reply);
        } else {
            reply->deleteLater();
            m_activeReply = nullptr;
            m_activeToken.clear();
            pumpNext();
        }
    });
}

void FileTransferManager::onPutFinished(Task &task, QNetworkReply *reply)
{
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError networkError = reply->error();
    const QString detail = reply->errorString();
    reply->deleteLater();
    m_activeReply = nullptr;
    m_activeToken.clear();

    if (task.cancelled) {
        pumpNext();
        return;
    }

    const int index = task.activeIndex;
    task.activeIndex = -1;

    if (networkError == QNetworkReply::NoError && status == 200) {
        task.attempts = 0;
        task.remainingChunks.remove(index);
        // 进度按密文字节计（与 taskProgress 的 total=cipherSize 同口径）
        task.bytesDone = qMin(task.cipherSize, task.bytesDone + task.chunkSize);
        emit taskProgress(task.token, task.phase, task.bytesDone, task.cipherSize);
        pumpUpload(task);
        pumpNext();
        return;
    }

    // 失败：先问服务端到底收了哪些片（响应可能丢失而数据已落盘），
    // 再决定跳过还是重传，避免重复上传已成功的分片
    ++task.attempts;
    ++task.recoveryRounds;
    if (isTransientFailure(networkError, status) && task.attempts <= MaxChunkAttempts
        && task.recoveryRounds <= MaxRecoveryRounds) {
        task.phase = QLatin1String("recovering");
        emit uploadQueryRequested(requestSeq(task.token), task.fileId);
        return;
    }
    failTask(task.token,
             QLatin1String("Failed to upload chunk ") + QString::number(index)
                 + QLatin1String(" (HTTP ") + QString::number(status)
                 + QLatin1String(": ") + detail + QLatin1String(")"));
}

void FileTransferManager::onUploadQueried(qint64 seq, bool ok, const QList<int> &receivedChunks,
                                          const QString &error)
{
    Task *task = taskBySeq(seq);
    if (!task) {
        return;
    }
    if (task->cancelled) {
        pumpNext();
        return;
    }
    if (ok) {
        // 以服务端实际落盘的分片为准刷新待传集合：已收的不再重传
        for (int index : receivedChunks) {
            task->remainingChunks.remove(index);
        }
        // 不清零 attempts：数据面持续 5xx 而控制面正常时查询总会成功，
        // 用它清零重试预算会形成"上传-失败-查询-重上传"的活锁（永不放弃、
        // UI 无失败态、持续向服务端灌 4 MiB 请求体）。重试预算只由分片
        // 上传成功清零，总恢复轮次由 recoveryRounds 封顶
        task->phase = QLatin1String("uploading");
        pumpNext();
        return;
    }
    // 查询也失败：重试预算已耗尽则放弃，否则直接重传当前分片
    if (task->attempts > MaxChunkAttempts || task->recoveryRounds > MaxRecoveryRounds) {
        failTask(task->token,
                 error.isEmpty() ? QLatin1String("Upload recovery failed") : error);
        return;
    }
    pumpNext();
}

void FileTransferManager::onUploadCompleted(qint64 seq, bool ok, const QString &error)
{
    Task *task = taskBySeq(seq);
    if (!task) {
        return;
    }
    if (task->cancelled) {
        finishTask(task->token);
        return;
    }
    if (!ok) {
        failTask(task->token, error.isEmpty() ? QLatin1String("Failed to complete upload") : error);
        return;
    }

    // 组装清单（明文，含文件密钥），交给 NetworkManager 经既有 E2EE 加密后
    // 作为消息正文发送。清单绝不进 QML
    FileManifest manifest;
    manifest.fileId = task->fileId;
    manifest.name = task->fileName;
    manifest.mime = task->mime;
    manifest.plainSize = task->plainSize;
    manifest.cipherSize = task->cipherSize;
    manifest.chunkSize = task->chunkSize;
    manifest.sha256Hex = task->sha256Hex;
    manifest.key = task->fileKey;
    manifest.iv = task->iv;
    // M8.3: 多媒体元数据（图片尺寸与内联缩略图；M8.3b 音视频时长与视频封面）。
    // 这些字段只存在于清单里，随消息正文经 E2EE 分发，服务端全程不可见
    manifest.width = task->mediaWidth;
    manifest.height = task->mediaHeight;
    manifest.durationMs = task->mediaDurationMs;
    manifest.thumbnail = task->thumbnail;
    const QString manifestJson = Protocol::encodeFileManifest(manifest);
    if (manifestJson.isEmpty()) {
        // 清单非法意味着上面的字段自相矛盾（如口径不自洽），宁可不发也不要
        // 投出一条接收端解不开的清单
        failTask(task->token, QLatin1String("Failed to build the file manifest"));
        return;
    }

    // 本人发送的文件同样需要能被本人其他设备下载，但登记以 messageId 为键，
    // 而 messageId 要到发送成功后才存在：由 NetworkManager 在收到发送响应后
    // 调 registerIncomingFile 完成登记，本处不预先登记
    emit sendMessageRequested(task->conversationId, task->peerUserId, manifestJson, task->fileId);
    emit taskProgress(task->token, QLatin1String("sent"), task->cipherSize, task->cipherSize);
    finishTask(task->token);
}

void FileTransferManager::onUploadCancelled(qint64 seq, bool ok, const QString &error)
{
    Q_UNUSED(ok);
    Q_UNUSED(error);
    Task *task = taskBySeq(seq);
    if (!task) {
        return;
    }
    // 取消结果不影响本地：无论服务端是否成功回收分片，任务都已终止
    //（回收失败由服务端维护任务兜底）
    finishTask(task->token);
}

void FileTransferManager::cancelTask(const QString &token)
{
    const auto it = m_tasks.find(token);
    if (it == m_tasks.end()) {
        return;
    }
    Task &task = it.value();
    if (task.cancelled) {
        return;
    }
    task.cancelled = true;
    task.phase = QLatin1String("cancelling");
    // M8.3b: 如果取消的任务正在提取元数据或在队列中，清理提取状态
    clearExtractionStateForToken(token);
    // P4.4/P4.3: 若正在做本地工作（hashing/saving），关闭句柄并复位泵队列，
    // 使随后对 tmpPath 的删除不会因句柄未关而失败
    clearLocalStateForToken(token);

    // 立即中止在途请求，不等它自然结束
    if (m_activeReply && m_activeToken == token) {
        m_activeReply->abort();
    }
    if (!task.tmpPath.isEmpty()) {
        QFile::remove(task.tmpPath);
        task.tmpPath.clear();
    }

    if (task.isUpload && task.fileId > 0) {
        // 请求控制面取消，回收服务端已收分片（否则白占并发配额与磁盘 48 小时）
        emit uploadCancelRequested(requestSeq(token), task.fileId);
        return;
    }
    finishTask(token);
}

QString FileTransferManager::download(qint64 messageId)
{
    Task task;
    task.token = makeToken();
    task.isUpload = false;
    task.messageId = messageId;
    const QString token = task.token;
    m_tasks.insert(token, task);
    emit tasksChanged();

    if (!isEnabled()) {
        failTask(token, QLatin1String("File transfer is not available on this server"));
        return token;
    }
    FileManifest manifest;
    if (!manifestFor(messageId, &manifest)) {
        failTask(token, QLatin1String("File metadata is not available for this message"));
        return token;
    }

    Task &t = m_tasks[token];
    t.fileName = manifest.name;   // P4.1: 横幅展示真实文件名（下载任务此前无名）
    t.sha256Hex = manifest.sha256Hex;
    t.cipherSize = manifest.cipherSize;
    t.chunkSize = manifest.chunkSize;
    t.chunkCount = Protocol::chunkCountFor(manifest.cipherSize, manifest.chunkSize);

    // 缓存命中：密文原样落盘即可复用，免一次下载
    if (isCached(manifest.sha256Hex, manifest.cipherSize)) {
        emit downloadStateChanged(messageId, QLatin1String("available"));
        emit taskProgress(token, QLatin1String("cached"), manifest.cipherSize, manifest.cipherSize);
        finishTask(token);
        return token;
    }
    if (t.chunkCount <= 0 || t.chunkCount > Protocol::MaxChunkCount) {
        failTask(token, QLatin1String("File chunking is invalid"));
        return token;
    }

    t.phase = QLatin1String("requesting");
    emit downloadStateChanged(messageId, QLatin1String("downloading"));
    emit downloadTicketRequested(requestSeq(token), manifest.fileId);
    return token;
}

void FileTransferManager::onDownloadTicket(qint64 seq, bool ok, const QString &ticket,
                                           qint64 sizeBytes, qint64 chunkSize, int chunkCount,
                                           const QString &sha256Hex, const QString &error)
{
    Task *task = taskBySeq(seq);
    if (!task) {
        return;
    }
    if (!ok || ticket.isEmpty()) {
        emit downloadStateChanged(task->messageId, QLatin1String("missing"));
        failTask(task->token,
                 error.isEmpty() ? QLatin1String("Failed to get a download ticket") : error);
        return;
    }

    // 服务端声明的口径必须与清单（经 E2EE 的可信元数据）完全一致。
    // 不一致说明服务端记录被换过或它在撒谎：即便 GCM 最终会拒绝错误密文，
    // 也不该浪费一次完整下载才发现
    if (sizeBytes != task->cipherSize || chunkSize != task->chunkSize
        || chunkCount != task->chunkCount || sha256Hex != task->sha256Hex) {
        emit downloadStateChanged(task->messageId, QLatin1String("missing"));
        failTask(task->token, QLatin1String("Server metadata does not match the manifest"));
        return;
    }
    if (!ensureCacheRoot()) {
        emit downloadStateChanged(task->messageId, QLatin1String("missing"));
        failTask(task->token, QLatin1String("Cannot create the local cache directory"));
        return;
    }

    const QString finalPath = cachePathFor(task->sha256Hex);
    QDir().mkpath(QFileInfo(finalPath).absolutePath());
    // 临时名带随机后缀：同一文件被并发下载两次也不会互相覆写
    task->tmpPath = finalPath + QLatin1String(".")
        + QUuid::createUuid().toString(QUuid::Id128) + QLatin1String(".tmp");
    task->downloadTicket = ticket;
    task->downloadIndex = 0;
    task->bytesDone = 0;
    task->phase = QLatin1String("downloading");
    pumpNext();
}

void FileTransferManager::pumpDownload(Task &task)
{
    if (task.cancelled || m_activeReply || task.tmpPath.isEmpty()) {
        return;
    }
    if (task.downloadIndex >= task.chunkCount) {
        QString error;
        if (!finalizeDownload(task, &error)) {
            emit downloadStateChanged(task.messageId, QLatin1String("missing"));
            failTask(task.token, error);
            return;
        }
        emit downloadStateChanged(task.messageId, QLatin1String("available"));
        finishTask(task.token);
        return;
    }

    // 按分片边界取：分片是独立 AEAD 加密的，客户端只能逐片解密，
    // 且服务端对单次 GET 有字节上限
    const qint64 start = task.chunkSize * task.downloadIndex;
    const qint64 expected = Protocol::expectedChunkBytes(task.cipherSize, task.chunkSize,
                                                         task.chunkCount, task.downloadIndex);
    if (expected <= 0) {
        emit downloadStateChanged(task.messageId, QLatin1String("missing"));
        failTask(task.token, QLatin1String("File chunking is invalid"));
        return;
    }
    // 清单可能在传输期间被 reset 清掉（登出/断线），此时 fileId 为 0，
    // 不得拼出一个指向 /file/0 的请求
    FileManifest taskManifest;
    if (!manifestFor(task.messageId, &taskManifest) || taskManifest.fileId <= 0) {
        emit downloadStateChanged(task.messageId, QLatin1String("missing"));
        failTask(task.token, QLatin1String("File metadata is no longer available"));
        return;
    }
    const QUrl url(m_baseUrl + QLatin1String("/") + QString::number(taskManifest.fileId));
    QNetworkRequest request = makeRequest(url);
    request.setRawHeader(TicketHeader, task.downloadTicket.toLatin1());
    request.setRawHeader("Range",
                         QByteArray("bytes=") + QByteArray::number(start) + QByteArray("-")
                             + QByteArray::number(start + expected - 1));

    task.activeIndex = task.downloadIndex;
    m_activeToken = task.token;
    m_activeReply = m_nam->get(request);
    connect(m_activeReply, &QNetworkReply::finished, this, [this, reply = m_activeReply]() {
        Task *current = m_tasks.find(m_activeToken) == m_tasks.end()
            ? nullptr
            : &m_tasks[m_activeToken];
        if (current) {
            onGetFinished(*current, reply);
        } else {
            reply->deleteLater();
            m_activeReply = nullptr;
            m_activeToken.clear();
            pumpNext();
        }
    });
}

void FileTransferManager::onGetFinished(Task &task, QNetworkReply *reply)
{
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray data = reply->readAll();
    const QNetworkReply::NetworkError networkError = reply->error();
    reply->deleteLater();
    m_activeReply = nullptr;
    m_activeToken.clear();

    if (task.cancelled) {
        pumpNext();
        return;
    }

    // 206 是分段响应，200 是服务端忽略了 Range 直接给了全量（后者只在
    // 单片文件时可能出现，长度校验会兜住）
    const bool statusOk = (status == 206 || status == 200) && networkError == QNetworkReply::NoError;
    if (!statusOk) {
        ++task.attempts;
        if (isTransientFailure(networkError, status) && task.attempts <= MaxChunkAttempts) {
            task.phase = QLatin1String("recovering");
            pumpDownload(task);
            return;
        }
        emit downloadStateChanged(task.messageId, QLatin1String("missing"));
        failTask(task.token,
                 QLatin1String("Failed to download chunk ") + QString::number(task.activeIndex)
                     + QLatin1String(" (HTTP ") + QString::number(status) + QLatin1String(")"));
        return;
    }

    const qint64 expected = Protocol::expectedChunkBytes(task.cipherSize, task.chunkSize,
                                                         task.chunkCount, task.activeIndex);
    if (static_cast<qint64>(data.size()) != expected) {
        // 长度不符即视为数据面故障：截断的密文会让整体摘要校验失败，
        // 但那时已无法定位是哪一片坏了
        emit downloadStateChanged(task.messageId, QLatin1String("missing"));
        failTask(task.token, QLatin1String("Downloaded chunk has an unexpected length"));
        return;
    }

    QFile out(task.tmpPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Append)) {
        emit downloadStateChanged(task.messageId, QLatin1String("missing"));
        failTask(task.token, QLatin1String("Cannot write to the local cache"));
        return;
    }
    const qsizetype written = out.write(data);
    out.close();
    if (written != data.size()) {
        emit downloadStateChanged(task.messageId, QLatin1String("missing"));
        failTask(task.token, QLatin1String("Failed to write the local cache"));
        return;
    }

    task.attempts = 0;
    task.bytesDone += data.size();
    ++task.downloadIndex;
    emit taskProgress(task.token, task.phase, task.bytesDone, task.cipherSize);
    pumpDownload(task);
    pumpNext();
}

bool FileTransferManager::finalizeDownload(Task &task, QString *error)
{
    QFile tmp(task.tmpPath);
    if (!tmp.exists()) {
        *error = QLatin1String("Downloaded data is missing");
        return false;
    }
    if (tmp.size() != task.cipherSize) {
        tmp.remove();
        *error = QLatin1String("Downloaded size does not match the manifest");
        return false;
    }
    if (!tmp.open(QIODevice::ReadOnly)) {
        *error = QLatin1String("Cannot read the downloaded data");
        return false;
    }
    // 整体摘要必须与清单一致：这是"服务端 blob 就是发送方加密的那份"的唯一凭据
    FileCrypto::Sha256Stream digest;
    while (!tmp.atEnd()) {
        digest.addData(tmp.read(1024 * 1024));
    }
    tmp.close();
    if (digest.hexDigest() != task.sha256Hex) {
        tmp.remove();
        *error = QLatin1String("Downloaded file failed the integrity check");
        return false;
    }

    // 原子改名进缓存：临时名到最终名在同一目录内，rename 不会跨卷
    const QString finalPath = cachePathFor(task.sha256Hex);
    if (QFileInfo::exists(finalPath)) {
        // 已有同内容缓存（另一任务先完成）：丢弃本次临时文件即可
        tmp.remove();
    } else if (!tmp.rename(finalPath)) {
        tmp.remove();
        *error = QLatin1String("Cannot move the downloaded data into the cache");
        return false;
    }
    task.tmpPath.clear();
    return true;
}

QString FileTransferManager::saveToFile(qint64 messageId, const QVariant &destPathOrUrl)
{
    // 同 uploadAndSend：保存对话框给的也是 file URL
    const QString destPath = toLocalPath(destPathOrUrl);

    Task task;
    task.token = makeToken();
    task.isUpload = false;
    task.messageId = messageId;
    const QString token = task.token;
    m_tasks.insert(token, task);
    emit tasksChanged();

    // 校验：清单/缓存/目标路径。任一不满足即同步 failTask（与 uploadAndSend 同口径：
    // 参数非法也返回 token，便于 UI 统一挂进度并收 taskFailed）
    FileManifest manifest;
    if (!manifestFor(messageId, &manifest)) {
        failTask(token, QLatin1String("File metadata is not available for this message"));
        return token;
    }
    const QString cachePath = cachePathFor(manifest.sha256Hex);
    if (!isCached(manifest.sha256Hex, manifest.cipherSize)) {
        failTask(token, QLatin1String("File is not downloaded yet"));  // UI 应先触发 download
        return token;
    }
    if (destPath.isEmpty()) {
        failTask(token, QLatin1String("Invalid destination path"));
        return token;
    }

    // P4.3: 建 SaveState（文件句柄在首个切片惰性打开），入本地泵队列由时间片
    // 增量解密写盘。明文只写目标临时文件，完成后原子改名，避免半截明文
    auto *st = new SaveState;
    st->destPath = destPath;
    st->cachePath = cachePath;
    st->tmpPath = destPath + QLatin1String(".xysaving");
    st->manifest = manifest;
    m_saveStates.insert(token, st);

    Task &t = m_tasks[token];
    t.phase = QLatin1String("saving");
    t.fileName = manifest.name;   // 横幅展示真实文件名
    t.cipherSize = manifest.cipherSize;
    t.chunkSize = manifest.chunkSize;
    t.plainSize = manifest.plainSize;
    t.tmpPath = st->tmpPath;      // 便于 cancel/finish/fail 统一清理半截明文
    emit taskProgress(token, t.phase, 0, manifest.cipherSize);
    enqueueLocalWork(token);
    return token;
}

void FileTransferManager::reset()
{
    // abort() 会同步触发 finished 回调：先立护栏并把所有任务标为已取消，
    // 再断开回调并清空在途指针，避免登出中途发起新请求或向 UI 弹失败。
    // m_activeReply 必须在此显式复位（不能只依赖回调）：否则残留非空会
    // 使 pumpNext 永远早退，新会话里所有传输都卡死
    m_resetting = true;
    // P4.4/P4.3: 先停本地泵并释放 hashing/save 中间态（关闭 QFile 句柄），
    // 使随后的任务循环能顺利删除保存临时文件（Windows 下打开的文件删不掉）
    if (m_localPump) {
        m_localPump->stop();
    }
    m_localToken.clear();
    m_pendingLocalTokens.clear();
    for (auto hit = m_hashStates.begin(); hit != m_hashStates.end(); ++hit) {
        HashState *st = hit.value();
        if (st->file.isOpen()) {
            st->file.close();
        }
        delete st;
    }
    m_hashStates.clear();
    for (auto sit = m_saveStates.begin(); sit != m_saveStates.end(); ++sit) {
        SaveState *sv = sit.value();
        if (sv->src.isOpen()) {
            sv->src.close();
        }
        if (sv->dst.isOpen()) {
            sv->dst.close();
        }
        SecureMemory::wipe(sv->manifest.key);
        delete sv;
    }
    m_saveStates.clear();
    for (auto it = m_tasks.begin(); it != m_tasks.end(); ++it) {
        it->cancelled = true;
        if (!it->tmpPath.isEmpty()) {
            QFile::remove(it->tmpPath);
        }
        // 文件密钥用后清零
        SecureMemory::wipe(it->fileKey);
    }
    if (m_activeReply) {
        disconnect(m_activeReply, nullptr, this, nullptr);
        m_activeReply->abort();
        m_activeReply->deleteLater();
        m_activeReply = nullptr;
    }
    m_activeToken.clear();
    m_tasks.clear();
    m_seqToToken.clear();
    // M8.3b: 清理元数据提取状态（extractor 取消、队列清空），避免悬空 token
    if (m_metadataExtractor) {
        m_metadataExtractor->cancel();
    }
    m_extractingToken.clear();
    m_pendingExtractTokens.clear();
    // 缩略图负缓存也属于会话态，登出/断线时清空
    m_thumbnailGenFailed.clear();
    // 已登记清单同样含密钥，不得跨会话驻留
    {
        QMutexLocker locker(&m_manifestMutex);
        for (auto it = m_incoming.begin(); it != m_incoming.end(); ++it) {
            SecureMemory::wipe(it->key);
        }
        m_incoming.clear();
    }
    m_resetting = false;
    emit tasksChanged();
}

QNetworkRequest FileTransferManager::makeRequest(const QUrl &url) const
{
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    // 复用主通道的 CA 与校验口径：数据面用同一套开发自签证书，
    // 不在此处放宽校验（不忽略 sslErrors）。未配置 CA 时不覆盖请求默认
    // 配置（生产环境用公共 CA 签发的证书即走此路径）
    if (!m_sslConfig.caCertificates().isEmpty()) {
        request.setSslConfiguration(m_sslConfig);
    }
    return request;
}

void FileTransferManager::finishTask(const QString &token)
{
    auto it = m_tasks.find(token);
    if (it == m_tasks.end()) {
        return;
    }
    // token 往往就是容器内 Task::token 的引用（调用点写的是 task.token）：
    // erase 之后它会悬垂，而随后的 emit 还要读它。必须先取一份独立副本
    // （拷贝构造使引用计数 +1，节点销毁后数据仍活）再销毁节点
    const QString tokenCopy = token;
    // M8.3b: 清理提取状态（防御性：finishTask 通常在任务完成后调用，
    // 此时已过 extracting 阶段，但意外路径下可能仍有悬空 token）
    clearExtractionStateForToken(tokenCopy);
    // P4.4/P4.3: 关闭并释放本地工作中间态（hashing/save 的 QFile 句柄与密钥）。
    // 必须在删 tmpPath 之前：Windows 下句柄未关时临时文件删不掉
    clearLocalStateForToken(tokenCopy);
    SecureMemory::wipe(it->fileKey);
    if (!it->tmpPath.isEmpty()) {
        QFile::remove(it->tmpPath);
    }
    m_tasks.erase(it);
    emit taskFinished(tokenCopy);
    emit tasksChanged();
    // 本任务释放了传输槽，推进其他等待中的任务（嵌套时由 m_pumping 拦住）
    pumpNext();
}

void FileTransferManager::failTask(const QString &token, const QString &error)
{
    auto it = m_tasks.find(token);
    if (it == m_tasks.end()) {
        return;
    }
    // 同 finishTask：先取 token 副本再 erase，避开悬垂引用
    const QString tokenCopy = token;
    // M8.3b: 同 finishTask，清理提取状态
    clearExtractionStateForToken(tokenCopy);
    // P4.4/P4.3: 同 finishTask，关闭并释放本地工作中间态（先于删 tmpPath）
    clearLocalStateForToken(tokenCopy);
    SecureMemory::wipe(it->fileKey);
    if (!it->tmpPath.isEmpty()) {
        QFile::remove(it->tmpPath);
    }
    m_tasks.erase(it);
    emit taskFailed(tokenCopy, error);
    emit tasksChanged();
    pumpNext();
}

} // namespace XYChat::Client
