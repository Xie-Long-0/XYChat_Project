#include "FileTransferManager.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
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

int FileTransferManager::activeTaskCount() const
{
    return static_cast<int>(m_tasks.size());
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
    const QStringList buckets = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &bucket : buckets) {
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
    }
    t.phase = QLatin1String("hashing");

    QString error;
    if (!computeCipherDigest(t, &error)) {
        failTask(token, error);
        return token;
    }

    t.phase = QLatin1String("creating");
    emit taskProgress(token, t.phase, 0, t.cipherSize);
    emit uploadCreateRequested(requestSeq(token), t.cipherSize, t.chunkSize, t.chunkCount,
                               t.sha256Hex);
    return token;
}

bool FileTransferManager::computeCipherDigest(Task &task, QString *error)
{
    QFile file(task.localPath);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QLatin1String("Cannot open file for reading");
        return false;
    }

    const FileCrypto::FileKey key = FileCrypto::generateFileKey();
    if (!key.valid) {
        *error = QLatin1String("Failed to generate a file key");
        return false;
    }
    task.fileKey = key.key;
    task.iv = key.iv;

    // 明文分片 = 密文分片 - GCM 标签，使每片密文恰好等于 chunkSize
    const qint64 plainChunk = Protocol::plainSizeOfChunk(task.chunkSize);
    if (plainChunk <= 0) {
        *error = QLatin1String("Invalid chunk size");
        return false;
    }

    // 第一遍只算摘要、不落临时文件：加密是确定性的（nonce 由 iv 与序号派生），
    // 第二遍上传时重算得到的密文与这里逐字节相同。代价是多一遍 AES 运算，
    // 换来磁盘占用不翻倍，也没有崩溃残留需要清理
    FileCrypto::Sha256Stream digest;
    qint64 cipherSize = 0;
    int chunkCount = 0;
    qint64 offset = 0;
    while (offset < task.plainSize) {
        const qint64 want = qMin(plainChunk, task.plainSize - offset);
        const QByteArray plain = file.read(want);
        if (plain.size() != want) {
            *error = QLatin1String("Failed to read file");
            return false;
        }
        const QByteArray cipher = FileCrypto::encryptChunk(task.fileKey, task.iv, chunkCount, plain);
        // 空明文分片也会产出标签，故成功时密文必不为空
        if (cipher.isEmpty()) {
            *error = QLatin1String("Failed to encrypt file");
            return false;
        }
        digest.addData(cipher);
        cipherSize += cipher.size();
        offset += want;
        ++chunkCount;
        emit taskProgress(task.token, QLatin1String("hashing"), offset, task.plainSize);
    }
    file.close();

    if (cipherSize > Protocol::MaxFileSize) {
        *error = QLatin1String("File exceeds the maximum size");
        return false;
    }
    if (chunkCount > Protocol::MaxChunkCount) {
        *error = QLatin1String("File has too many chunks");
        return false;
    }
    if (!Protocol::isChunkingValid(cipherSize, task.chunkSize, chunkCount)) {
        *error = QLatin1String("Chunking parameters are inconsistent");
        return false;
    }

    task.cipherSize = cipherSize;
    task.chunkCount = chunkCount;
    task.sha256Hex = digest.hexDigest();
    return true;
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
    // M8.3: 多媒体元数据（图片尺寸与内联缩略图）。这些字段只存在于清单里，
    // 随消息正文经 E2EE 分发，服务端全程不可见
    manifest.width = task->mediaWidth;
    manifest.height = task->mediaHeight;
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

bool FileTransferManager::saveToFile(qint64 messageId, const QVariant &destPathOrUrl)
{
    // 同 uploadAndSend：保存对话框给的也是 file URL
    const QString destPath = toLocalPath(destPathOrUrl);
    FileManifest manifest;
    if (!manifestFor(messageId, &manifest)) {
        return false;
    }
    const QString cachePath = cachePathFor(manifest.sha256Hex);
    if (!isCached(manifest.sha256Hex, manifest.cipherSize)) {
        return false;  // UI 应先触发 download
    }
    if (destPath.isEmpty()) {
        return false;
    }

    QFile src(cachePath);
    if (!src.open(QIODevice::ReadOnly)) {
        return false;
    }
    // 明文只写用户显式选择的目标路径，且经临时文件 + 改名，避免半截明文
    const QString tmpPath = destPath + QLatin1String(".xysaving");
    QFile dst(tmpPath);
    if (!dst.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        src.close();
        return false;
    }

    qint64 offset = 0;
    qint64 written = 0;
    int index = 0;
    // 两类失败必须分开：缓存损坏（读侧/GCM 认证失败）应当删缓存让用户重下；
    // 目标写失败（磁盘满/无权限/被杀软锁定）时缓存是完好的，删它只会
    // 迫使重下整个文件且大概率再次失败
    bool cacheCorrupt = false;
    bool destFailed = false;
    while (offset < manifest.cipherSize) {
        const qint64 want = qMin(manifest.chunkSize, manifest.cipherSize - offset);
        const QByteArray cipher = src.read(want);
        if (cipher.size() != want) {
            cacheCorrupt = true;
            break;
        }
        const QByteArray plain = FileCrypto::decryptChunk(manifest.key, manifest.iv, index, cipher);
        // 密文分片恰好只有一个标签长时，明文为空是合法结果；其余情况下
        // 空返回值意味着 GCM 认证失败（缓存损坏或密钥不符）
        if (plain.isEmpty() && want > GcmTagSize) {
            cacheCorrupt = true;
            break;
        }
        if (dst.write(plain) != plain.size()) {
            destFailed = true;
            break;
        }
        written += plain.size();
        offset += want;
        ++index;
        // 本函数为同步实现（逐片解密直写目标文件），不归属于任何传输任务，
        // 因此不发 taskProgress。大文件保存会短时阻塞调用线程，已登记为欠账
    }
    src.close();
    dst.close();

    if (cacheCorrupt || destFailed || written != manifest.plainSize) {
        dst.remove();
        if (cacheCorrupt || written != manifest.plainSize) {
            // 缓存已损坏（GCM 认证失败或长度不符）：删掉它并把状态改回缺失，
            // 让 UI 重新下载，否则用户会反复撞上同一个坏文件
            QFile::remove(cachePath);
            emit downloadStateChanged(messageId, QLatin1String("missing"));
        }
        return false;
    }
    if (QFileInfo::exists(destPath)) {
        QFile::remove(destPath);
    }
    if (!QFile::rename(tmpPath, destPath)) {
        QFile::remove(tmpPath);
        return false;
    }
    emit fileSaved(messageId, destPath);
    return true;
}

void FileTransferManager::reset()
{
    // abort() 会同步触发 finished 回调：先立护栏并把所有任务标为已取消，
    // 再断开回调并清空在途指针，避免登出中途发起新请求或向 UI 弹失败。
    // m_activeReply 必须在此显式复位（不能只依赖回调）：否则残留非空会
    // 使 pumpNext 永远早退，新会话里所有传输都卡死
    m_resetting = true;
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
