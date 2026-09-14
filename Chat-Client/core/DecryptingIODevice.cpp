#include "DecryptingIODevice.h"

#include <cstring>

#include "FileCrypto.h"
#include "FileTransferManager.h"
#include "SecureMemory.h"

namespace XYChat::Client
{

using XYChat::Security::FileCrypto;
using XYChat::Security::SecureMemory;
namespace Protocol = XYChat::Protocol;

namespace
{
// GCM 标签长度：明文分片 = 密文分片 - 标签
constexpr qint64 GcmTagSize = 16;
}

DecryptingIODevice::DecryptingIODevice(qint64 messageId, FileTransferManager *transfer,
                                       QObject *parent)
    : QIODevice(parent)
    , m_messageId(messageId)
    , m_transfer(transfer)
{
}

DecryptingIODevice::~DecryptingIODevice()
{
    close();
    // 清单含文件密钥，析构时清零（close 已 wipe，此处兜底防 close 未被调用）
    SecureMemory::wipe(m_manifest.key);
}

bool DecryptingIODevice::open(OpenMode mode)
{
    // open 在 GUI 线程调用（setSourceDevice 之前，后端尚未开始读取）；
    // 加锁与 close/readData/seek 统一口径。playbackInfoForMessage 内部会加
    // FileTransferManager 的清单锁，锁顺序 device→transfer，无反向不死锁
    QMutexLocker locker(&m_mutex);
    if (m_opened) {
        return true;
    }
    // 只支持纯只读：播放器不需要写，ReadWrite/WriteOnly 一律拒绝
    if (!(mode & ReadOnly) || (mode & WriteOnly)) {
        return false;
    }
    if (!m_transfer || m_messageId <= 0) {
        return false;
    }
    QString cachePath;
    // playbackInfoForMessage 内部加锁拷贝清单，并校验缓存已就绪
    if (!m_transfer->playbackInfoForMessage(m_messageId, &m_manifest, &cachePath)) {
        return false;
    }
    m_cacheFile.setFileName(cachePath);
    if (!m_cacheFile.open(QIODevice::ReadOnly)) {
        SecureMemory::wipe(m_manifest.key);
        return false;
    }
    m_plainChunkSize = m_manifest.chunkSize - GcmTagSize;
    m_chunkCount = Protocol::chunkCountFor(m_manifest.cipherSize, m_manifest.chunkSize);
    if (m_plainChunkSize <= 0 || m_chunkCount <= 0
        || !Protocol::isChunkingValid(m_manifest.cipherSize, m_manifest.chunkSize,
                                      m_chunkCount)) {
        m_cacheFile.close();
        SecureMemory::wipe(m_manifest.key);
        return false;
    }
    m_opened = true;
    m_plainPos = 0;
    m_chunkBuffer.clear();
    m_chunkBufferOffset = 0;
    m_chunkPlainStart = -1;
    return QIODevice::open(mode);
}

void DecryptingIODevice::close()
{
    // 与后端线程的在途 readData 互斥：持锁后再置 m_opened=false 并 wipe，
    // readData 拿锁后会看到 m_opened=false 直接返回，不会读到已 wipe 的缓冲
    QMutexLocker locker(&m_mutex);
    if (!m_opened) {
        return;
    }
    m_opened = false;
    if (m_cacheFile.isOpen()) {
        m_cacheFile.close();
    }
    // 明文缓冲与密钥用后清零：缓冲里是解密后的音视频明文，不得驻留
    SecureMemory::wipe(m_chunkBuffer);
    m_chunkBuffer.clear();
    m_chunkBufferOffset = 0;
    m_chunkPlainStart = -1;
    m_plainPos = 0;
    SecureMemory::wipe(m_manifest.key);
    QIODevice::close();
}

qint64 DecryptingIODevice::size() const
{
    QMutexLocker locker(&m_mutex);
    return m_opened ? m_manifest.plainSize : 0;
}

qint64 DecryptingIODevice::bytesAvailable() const
{
    QMutexLocker locker(&m_mutex);
    if (!m_opened) {
        return 0;
    }
    return qMax<qint64>(0, m_manifest.plainSize - m_plainPos);
}

bool DecryptingIODevice::seek(qint64 pos)
{
    QMutexLocker locker(&m_mutex);
    if (!m_opened) {
        return false;
    }
    if (pos < 0 || pos > m_manifest.plainSize) {
        return false;
    }
    m_plainPos = pos;
    // 清空缓冲：下次 readData 会重新解密 pos 所在的片。不在此处预解密，
    // 因为 QMediaPlayer 可能连续 seek（拖动进度条），预解密会白做功
    SecureMemory::wipe(m_chunkBuffer);
    m_chunkBuffer.clear();
    m_chunkBufferOffset = 0;
    m_chunkPlainStart = -1;
    return QIODevice::seek(pos);
}

bool DecryptingIODevice::ensureChunkFor(qint64 plainPos)
{
    // plainPos 已在当前缓冲范围内：只需移动偏移，无需重新解密
    if (m_chunkPlainStart >= 0 && plainPos >= m_chunkPlainStart
        && plainPos < m_chunkPlainStart + m_chunkBuffer.size()) {
        m_chunkBufferOffset = plainPos - m_chunkPlainStart;
        return true;
    }
    // 计算 plainPos 所在的片索引：前面所有片明文都是 m_plainChunkSize 长，
    // 故 index = plainPos / m_plainChunkSize（最后一片可能较短，但起始位置一致）
    const int index = static_cast<int>(plainPos / m_plainChunkSize);
    if (index < 0 || index >= m_chunkCount) {
        return false;
    }
    const qint64 cipherLen = Protocol::expectedChunkBytes(m_manifest.cipherSize,
                                                          m_manifest.chunkSize,
                                                          m_chunkCount, index);
    if (cipherLen <= GcmTagSize) {
        return false;
    }
    const qint64 cipherOffset = m_manifest.chunkSize * static_cast<qint64>(index);
    if (!m_cacheFile.seek(cipherOffset)) {
        return false;
    }
    const QByteArray cipher = m_cacheFile.read(cipherLen);
    if (cipher.size() != cipherLen) {
        return false;  // 缓存被截断
    }
    const QByteArray plain = FileCrypto::decryptChunk(m_manifest.key, m_manifest.iv,
                                                      index, cipher);
    // 密文分片恰好只有一个标签长时明文为空是合法结果；其余情况下空返回值
    // 意味着 GCM 认证失败（缓存损坏或密钥不符）
    if (plain.isEmpty() && cipherLen > GcmTagSize) {
        return false;
    }
    // 旧缓冲里是上一片的明文，先清零再替换
    SecureMemory::wipe(m_chunkBuffer);
    m_chunkBuffer = plain;
    m_chunkPlainStart = m_plainChunkSize * static_cast<qint64>(index);
    // 定位到 plainPos 在本片内的偏移：seek 到片中间时（如 plainPos=1310720、
    // 本片起点 1048560）必须跳过片头已读过的部分，否则会错位返回片头数据
    m_chunkBufferOffset = plainPos - m_chunkPlainStart;
    return true;
}

qint64 DecryptingIODevice::readData(char *data, qint64 maxlen)
{
    // 后端解码线程调用：与 GUI 线程的 close/seek 互斥。持锁期间完成
    // ensureChunkFor（解密）与 memcpy，close 会等本次 readData 结束后才能 wipe，
    // 避免对已 clear 缓冲的读取。ensureChunkFor 不单独加锁（已在本锁内）
    QMutexLocker locker(&m_mutex);
    if (!m_opened || data == nullptr) {
        return -1;
    }
    if (maxlen <= 0) {
        return 0;
    }
    if (m_plainPos >= m_manifest.plainSize) {
        return 0;  // EOF
    }
    qint64 totalRead = 0;
    while (totalRead < maxlen && m_plainPos < m_manifest.plainSize) {
        if (!ensureChunkFor(m_plainPos)) {
            // 解密失败（缓存损坏/密钥不符）：已读到部分数据则返回实际字节数，
            // 让调用方下次再撞 EOF/错误；一片未读则返回 -1 明确报错
            return totalRead > 0 ? totalRead : -1;
        }
        const qint64 available = m_chunkBuffer.size() - m_chunkBufferOffset;
        if (available <= 0) {
            // 当前片已读完，推进到下一片起点，下轮循环重新解密
            m_plainPos = m_chunkPlainStart + m_chunkBuffer.size();
            SecureMemory::wipe(m_chunkBuffer);
            m_chunkBuffer.clear();
            m_chunkBufferOffset = 0;
            m_chunkPlainStart = -1;
            continue;
        }
        const qint64 want = qMin(maxlen - totalRead, available);
        std::memcpy(data + totalRead, m_chunkBuffer.constData() + m_chunkBufferOffset,
                    static_cast<size_t>(want));
        totalRead += want;
        m_chunkBufferOffset += want;
        m_plainPos += want;
    }
    return totalRead;
}

qint64 DecryptingIODevice::writeData(const char *, qint64)
{
    return -1;  // 只读设备，写一律失败
}

} // namespace XYChat::Client
