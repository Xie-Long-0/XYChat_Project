#include "LocalFileStorage.h"

#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStringList>

#include <algorithm>

#include "encryption/E2eeCrypto.h"
#include "encryption/FileCrypto.h"
#include "protocol/FileProtocol.h"

namespace XYChat::Server
{

namespace
{
const char PartsSuffix[] = ".part";
const char TmpSuffix[] = ".tmp";
const char BlobSuffix[] = ".bin";

// 分片索引上界与协议分片数上限一致，避免收到越界索引后在磁盘上建出海量文件
constexpr int MaxChunkIndex = XYChat::Protocol::MaxChunkCount - 1;

// 是否为小写十六进制串（blobKey 的形态校验）
bool isHexLower(const QString &value, int expectedLength)
{
    if (value.size() != expectedLength) {
        return false;
    }
    for (QChar c : value) {
        const bool isDigit = c >= '0' && c <= '9';
        const bool isLowerHex = c >= 'a' && c <= 'f';
        if (!isDigit && !isLowerHex) {
            return false;
        }
    }
    return true;
}
} // namespace

LocalFileStorage::LocalFileStorage(const QString &rootPath)
    : m_root(rootPath)
{
}

bool LocalFileStorage::initialize()
{
    return ensureDir(m_root.path() + "/parts")
        && ensureDir(m_root.path() + "/tmp")
        && ensureDir(m_root.path() + "/blobs");
}

QString LocalFileStorage::allocateBlobKey()
{
    const QByteArray raw = XYChat::Security::E2eeCrypto::generateRandomBytes(BlobKeyBytes);
    if (raw.size() != BlobKeyBytes) {
        // 随机数失败时不返回任何可预测的键：退化为时间戳或计数器会让存储路径
        // 变得可枚举，进而可被用于探测他人文件是否存在
        return {};
    }
    const QString blobKey = QString::fromLatin1(raw.toHex());
    return isValidBlobKey(blobKey) ? blobKey : QString();
}

bool LocalFileStorage::isValidBlobKey(const QString &blobKey) const
{
    return isHexLower(blobKey, BlobKeyLength);
}

bool LocalFileStorage::putChunk(const QString &blobKey, int index, const QByteArray &data)
{
    // 空分片直接拒绝：密文分片至少含 16 字节 GCM 标签，空数据只会是探测或误用，
    // 收下它会让 receivedChunks 报告一个 finalize 阶段必然长度不符的分片
    if (!isValidBlobKey(blobKey) || index < 0 || index > MaxChunkIndex || data.isEmpty()) {
        return false;
    }
    if (!ensureDir(partsDirPath(blobKey))) {
        return false;
    }

    // QSaveFile 走临时文件加改名，写一半崩溃不会留下被计入已收分片的残缺文件
    QSaveFile file(chunkFilePath(blobKey, index));
    file.setDirectWriteFallback(true);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    if (file.write(data) != data.size()) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

QList<int> LocalFileStorage::receivedChunks(const QString &blobKey)
{
    QList<int> indexes;
    if (!isValidBlobKey(blobKey)) {
        return indexes;
    }
    const QDir dir(partsDirPath(blobKey));
    if (!dir.exists()) {
        return indexes;
    }

    const QStringList entries = dir.entryList(QStringList{"*" + QLatin1String(PartsSuffix)},
                                              QDir::Files, QDir::Name);
    const int suffixLength = static_cast<int>(qstrlen(PartsSuffix));
    for (const QString &entry : entries) {
        if (entry.size() <= suffixLength) {
            continue;
        }
        bool ok = false;
        const int index = entry.chopped(suffixLength).toInt(&ok);
        // 目录中可能混入人工放置或旧版本遗留的文件，只接受形态正确的分片名
        if (ok && index >= 0 && index <= MaxChunkIndex) {
            indexes.append(index);
        }
    }
    std::sort(indexes.begin(), indexes.end());
    return indexes;
}

QByteArray LocalFileStorage::readChunk(const QString &blobKey, int index)
{
    if (!isValidBlobKey(blobKey) || index < 0 || index > MaxChunkIndex) {
        return {};
    }
    QFile file(chunkFilePath(blobKey, index));
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

LocalFileStorage::FinalizeStatus LocalFileStorage::finalize(const QString &blobKey,
                                                           qint64 cipherSize, qint64 chunkSize,
                                                           int chunkCount,
                                                           const QString &expectedSha256Hex)
{
    if (!isValidBlobKey(blobKey)) {
        return FinalizeStatus::InvalidArguments;
    }
    // 分片参数与校验和形态先行判定：不合法就不触碰磁盘
    if (!XYChat::Protocol::isChunkingValid(cipherSize, chunkSize, chunkCount)) {
        return FinalizeStatus::InvalidArguments;
    }
    if (!XYChat::Protocol::isSha256Hex(expectedSha256Hex)) {
        return FinalizeStatus::InvalidArguments;
    }

    // 同一 blobKey 的组装与删除串行：本实例由各连接线程共享，无锁时
    // 与 remove 并发会产出"元数据 ready 而对象已被删"的不可自愈状态
    QMutex &stripe = stripeFor(blobKey);
    QMutexLocker locker(&stripe);

    // 幂等：已组装过（完成请求重试，或上次在改名成功后、清理分片前崩溃）时
    // 体积一致即视为成功，不重复组装
    if (isFinalized(blobKey)) {
        return blobSize(blobKey) == cipherSize ? FinalizeStatus::Ok
                                               : FinalizeStatus::ChecksumMismatch;
    }

    // 分片必须恰好齐备且索引连续覆盖 0..chunkCount-1：
    // 只看数量会让"缺 0 号多一个越界号"这类组合蒙混过关
    const QList<int> received = receivedChunks(blobKey);
    if (received.size() != static_cast<qsizetype>(chunkCount)) {
        return FinalizeStatus::Incomplete;
    }
    for (int i = 0; i < chunkCount; ++i) {
        if (received.at(i) != i) {
            return FinalizeStatus::Incomplete;
        }
    }

    if (!ensureDir(tmpDirPath(blobKey))) {
        return FinalizeStatus::StorageError;
    }
    // 临时文件名带随机后缀：即使两次组装意外并发也不会共用同一路径而交错写入，
    // 崩溃残留也由 remove 按前缀枚举清理
    const QString tmpPath = newTmpFilePath(blobKey);
    if (tmpPath.isEmpty()) {
        return FinalizeStatus::StorageError;
    }

    QFile out(tmpPath);
    if (!out.open(QIODevice::WriteOnly)) {
        return FinalizeStatus::StorageError;
    }

    // 流式组装：逐片读入、写出并累加摘要，内存占用与分片大小同阶而非文件总大小
    XYChat::Security::FileCrypto::Sha256Stream digest;
    FinalizeStatus status = FinalizeStatus::Ok;
    qint64 written = 0;
    for (int i = 0; status == FinalizeStatus::Ok && i < chunkCount; ++i) {
        const QByteArray chunk = readChunk(blobKey, i);
        // 分片长度必须与声明严格一致：否则客户端可自行选择分片边界，
        // 用少量大分片绕过 MaxChunkCount 与 MaxFileSize 的联合约束
        const qint64 expected = XYChat::Protocol::expectedChunkBytes(cipherSize, chunkSize,
                                                                     chunkCount, i);
        if (static_cast<qint64>(chunk.size()) != expected) {
            status = FinalizeStatus::ChunkSizeMismatch;
            break;
        }
        if (out.write(chunk) != chunk.size()) {
            status = FinalizeStatus::StorageError; // 磁盘写满或设备故障
            break;
        }
        digest.addData(chunk);
        written += chunk.size();
    }
    out.close();

    if (status != FinalizeStatus::Ok) {
        // fail-closed：不产出最终对象，保留已收分片以便客户端重传缺失/损坏的那几片
        QFile::remove(tmpPath);
        return status;
    }
    if (written != cipherSize) {
        // 逐片长度都对得上却总量不符，只可能是声明体积与分片参数不自洽
        QFile::remove(tmpPath);
        return FinalizeStatus::ChunkSizeMismatch;
    }
    if (digest.hexDigest() != expectedSha256Hex) {
        QFile::remove(tmpPath);
        return FinalizeStatus::ChecksumMismatch;
    }

    if (!ensureDir(blobDirPath(blobKey))) {
        QFile::remove(tmpPath);
        return FinalizeStatus::StorageError;
    }
    const QString finalPath = blobFilePath(blobKey);
    QFile::remove(finalPath);
    if (!QFile::rename(tmpPath, finalPath)) {
        // 改名失败（不同文件系统之间或权限不足）时不得把中间产物当作最终对象暴露出去
        QFile::remove(tmpPath);
        return FinalizeStatus::StorageError;
    }

    // 组装成功后回收分片。此步失败不影响结果：最终对象已就位，
    // 残留分片由过期清理兜底
    QDir(partsDirPath(blobKey)).removeRecursively();
    return FinalizeStatus::Ok;
}

bool LocalFileStorage::isFinalized(const QString &blobKey)
{
    return isValidBlobKey(blobKey) && QFile::exists(blobFilePath(blobKey));
}

qint64 LocalFileStorage::blobSize(const QString &blobKey)
{
    if (!isFinalized(blobKey)) {
        return -1;
    }
    return QFileInfo(blobFilePath(blobKey)).size();
}

QByteArray LocalFileStorage::readRange(const QString &blobKey, qint64 offset, qint64 length)
{
    if (!isFinalized(blobKey) || offset < 0 || length <= 0) {
        return {};
    }
    QFile file(blobFilePath(blobKey));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const qint64 size = file.size();
    if (offset >= size) {
        return {};
    }
    // 尾部越界按可用量截断（HTTP Range 的标准语义），由调用方结合 blobSize
    // 决定回 206 还是 416
    const qint64 readable = qMin(length, size - offset);
    if (!file.seek(offset)) {
        return {};
    }
    const QByteArray data = file.read(readable);
    return data.size() == readable ? data : QByteArray();
}

bool LocalFileStorage::remove(const QString &blobKey)
{
    if (!isValidBlobKey(blobKey)) {
        return false;
    }

    // 与 finalize 同一把条带锁：避免"一边组装完成、另一边删掉刚产出的对象"
    QMutex &stripe = stripeFor(blobKey);
    QMutexLocker locker(&stripe);

    bool ok = true;
    // 非 const：removeRecursively() 会修改目录状态
    QDir partsDir(partsDirPath(blobKey));
    if (partsDir.exists()) {
        // 先删目录内的分片再删目录本身，removeRecursively 失败即上报，
        // 避免密文残片长期占盘
        ok = partsDir.removeRecursively() && ok;
    }
    // 临时文件名带随机后缀，此处按前缀枚举清理（含崩溃残留）
    const QStringList tmpFiles = existingTmpFiles(blobKey);
    for (const QString &tmpPath : tmpFiles) {
        ok = QFile::remove(tmpPath) && ok;
    }
    const QString finalPath = blobFilePath(blobKey);
    if (QFile::exists(finalPath)) {
        ok = QFile::remove(finalPath) && ok;
    }
    return ok;
}

QString LocalFileStorage::partsDirPath(const QString &blobKey) const
{
    return m_root.path() + "/parts/" + blobKey.left(2) + "/" + blobKey;
}

QString LocalFileStorage::chunkFilePath(const QString &blobKey, int index) const
{
    return partsDirPath(blobKey) + "/" + QString::number(index) + QLatin1String(PartsSuffix);
}

QString LocalFileStorage::tmpDirPath(const QString &blobKey) const
{
    return m_root.path() + "/tmp/" + blobKey.left(2);
}

QString LocalFileStorage::newTmpFilePath(const QString &blobKey) const
{
    // 随机后缀而非固定名：固定名在并发组装下会被两条线程共用，
    // 交错写入后的产物仍可能通过各自的摘要校验（摘要算的是读到的分片，
    // 不是落盘文件），从而把损坏对象推上 blobs
    const QByteArray suffix = XYChat::Security::E2eeCrypto::generateRandomBytes(4);
    if (suffix.size() != 4) {
        return {};
    }
    return tmpDirPath(blobKey) + "/" + blobKey + "."
        + QString::fromLatin1(suffix.toHex()) + QLatin1String(TmpSuffix);
}

QStringList LocalFileStorage::existingTmpFiles(const QString &blobKey) const
{
    const QDir dir(tmpDirPath(blobKey));
    if (!dir.exists()) {
        return {};
    }
    QStringList result;
    const QStringList entries = dir.entryList(
        QStringList{blobKey + ".*" + QLatin1String(TmpSuffix)}, QDir::Files, QDir::Name);
    for (const QString &entry : entries) {
        result.append(dir.filePath(entry));
    }
    return result;
}

QString LocalFileStorage::blobDirPath(const QString &blobKey) const
{
    return m_root.path() + "/blobs/" + blobKey.left(2);
}

QString LocalFileStorage::blobFilePath(const QString &blobKey) const
{
    return blobDirPath(blobKey) + "/" + blobKey + QLatin1String(BlobSuffix);
}

bool LocalFileStorage::ensureDir(const QString &path) const
{
    if (QDir(path).exists()) {
        return true;
    }
    return QDir().mkpath(path);
}

QMutex &LocalFileStorage::stripeFor(const QString &blobKey)
{
    // blobKey 为 32 位小写十六进制，取末 4 位（16 位随机值）取模即可均匀分布；
    // 形态已由 isValidBlobKey 保证，此处不对转换失败做特殊处理（落到 0 号条带）
    bool ok = false;
    const uint bucket = blobKey.right(4).toUInt(&ok, 16);
    return m_stripes[ok ? static_cast<int>(bucket % LockStripes) : 0];
}

} // namespace XYChat::Server
