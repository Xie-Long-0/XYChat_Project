#include "FileCrypto.h"

#include <QtEndian>

#include "E2eeCrypto.h"

namespace XYChat::Security
{

namespace
{
constexpr int FileKeySize = 32;
constexpr int NonceSize = 12;
constexpr int TicketBytes = 32;
// nonce 中留给分片序号的尾部长度（4 字节，可编码至 2^32-1 个分片，
// 远大于协议 MaxChunkCount 上限）
constexpr int IndexBytes = 4;

QString toHexLower(const QByteArray &data)
{
    return QString::fromLatin1(data.toHex());
}
} // namespace

FileCrypto::FileKey FileCrypto::generateFileKey()
{
    FileKey result;
    result.key = E2eeCrypto::generateRandomBytes(FileKeySize);
    result.iv = E2eeCrypto::generateRandomBytes(NonceSize);
    // 随机数失败时必须判为不可用：退化到全零密钥会让全部文件对任何持有
    // 密文的人可解，宁可拒绝上传
    result.valid = result.key.size() == FileKeySize && result.iv.size() == NonceSize;
    return result;
}

QByteArray FileCrypto::chunkNonce(const QByteArray &iv, int chunkIndex)
{
    if (iv.size() != NonceSize || chunkIndex < 0) {
        return {};
    }

    // 拷贝后写入：QByteArray 隐式共享，data() 会 detach 出私有副本，
    // 调用方的 iv 不被修改（同一 iv 要为每个分片重复派生 nonce）
    QByteArray nonce = iv;
    const quint32 beIndex = qToBigEndian(static_cast<quint32>(chunkIndex));
    const char *indexBytes = reinterpret_cast<const char *>(&beIndex);
    char *tail = nonce.data() + (NonceSize - IndexBytes);
    for (int i = 0; i < IndexBytes; ++i) {
        tail[i] = static_cast<char>(tail[i] ^ indexBytes[i]);
    }
    return nonce;
}

QByteArray FileCrypto::chunkAad(int chunkIndex)
{
    if (chunkIndex < 0) {
        return {};
    }
    const quint32 beIndex = qToBigEndian(static_cast<quint32>(chunkIndex));
    return QByteArray(reinterpret_cast<const char *>(&beIndex), IndexBytes);
}

QByteArray FileCrypto::encryptChunk(const QByteArray &key, const QByteArray &iv,
                                    int chunkIndex, const QByteArray &plaintext)
{
    const QByteArray nonce = chunkNonce(iv, chunkIndex);
    const QByteArray aad = chunkAad(chunkIndex);
    if (nonce.isEmpty() || aad.isEmpty()) {
        return {};
    }
    return E2eeCrypto::aesGcmEncryptAad(key, nonce, plaintext, aad);
}

QByteArray FileCrypto::decryptChunk(const QByteArray &key, const QByteArray &iv,
                                    int chunkIndex, const QByteArray &ciphertext)
{
    const QByteArray nonce = chunkNonce(iv, chunkIndex);
    const QByteArray aad = chunkAad(chunkIndex);
    if (nonce.isEmpty() || aad.isEmpty()) {
        return {};
    }
    return E2eeCrypto::aesGcmDecryptAad(key, nonce, ciphertext, aad);
}

QString FileCrypto::sha256Hex(const QByteArray &data)
{
    return toHexLower(QCryptographicHash::hash(data, QCryptographicHash::Sha256));
}

void FileCrypto::Sha256Stream::addData(const QByteArray &data)
{
    if (!data.isEmpty()) {
        m_hash.addData(data);
    }
}

QString FileCrypto::Sha256Stream::hexDigest() const
{
    return toHexLower(m_hash.result());
}

QString FileCrypto::generateTicket()
{
    const QByteArray raw = E2eeCrypto::generateRandomBytes(TicketBytes);
    if (raw.size() != TicketBytes) {
        return {};
    }
    // base64url 且省略填充：票据要经 URL 路径段与 JSON 传输，
    // 避开 +、/、= 的转义与截断歧义
    return QString::fromLatin1(raw.toBase64(QByteArray::Base64UrlEncoding
                                            | QByteArray::OmitTrailingEquals));
}

QString FileCrypto::ticketHash(const QString &ticket)
{
    if (ticket.isEmpty()) {
        return {};
    }
    return toHexLower(QCryptographicHash::hash(ticket.toUtf8(), QCryptographicHash::Sha256));
}

} // namespace XYChat::Security
