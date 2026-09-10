#pragma once

#include <QByteArray>
#include <QCryptographicHash>
#include <QString>

namespace XYChat::Security
{

/**
 * M8: 文件内容的端到端加密原语
 *
 * 密钥分发：每个文件一把独立的随机 AES-256 密钥与 12 字节 nonce 前缀，二者写入
 * FileManifest 后随消息正文经既有 E2EE 通道分发（私聊走 envelope、群聊走
 * Sender-Key），服务端只见密文，无法解出文件密钥。
 *
 * 分片独立加密：第 i 片的 nonce = iv XOR be32(i)、AAD = be32(i)。
 * - nonce 唯一性由 iv 的随机性与 XOR 对固定 iv 的双射性共同保证；
 * - AAD 绑定分片位置，重排、截断或以他片冒替均在 GCM 认证阶段被拒；
 * - 各片互相独立，因此上传/下载可流式进行、可断点续传、内存占用恒定。
 *
 * 刻意不复用消息 ratchet：文件密钥与 Sender Key 解耦后，分片没有必须按序消费
 * 的链状态，也就不会出现"链已推进导致早先分片永久不可解"这类不可逆损坏
 * （M9 消息编辑踩过的坑）；转发/多端重复下载同一文件也不需要重新加密。
 */
class FileCrypto
{
public:
    // 单个文件的密钥材料（原始字节，序列化进清单时转 base64）
    struct FileKey
    {
        QByteArray key; // 32 字节 AES-256 密钥
        QByteArray iv;  // 12 字节 nonce 前缀
        bool valid = false;
    };

    // 生成文件密钥与 nonce 前缀。每个文件独立生成，严禁跨文件复用同一密钥
    static FileKey generateFileKey();

    // 分片 nonce：iv 的后 4 字节与大端分片序号异或（与 TLS 1.3 记录层
    // "写 IV 异或序号" 的构造同构）。iv 长度不符或序号为负时返回空
    static QByteArray chunkNonce(const QByteArray &iv, int chunkIndex);

    // 分片 AAD：大端 4 字节分片序号。序号为负时返回空
    static QByteArray chunkAad(int chunkIndex);

    // 加密单个分片，输出 密文||16B 标签；密钥/nonce 非法或底层失败返回空。
    // 空明文分片也会产出 16 字节标签，故成功时返回值必不为空
    static QByteArray encryptChunk(const QByteArray &key, const QByteArray &iv,
                                   int chunkIndex, const QByteArray &plaintext);

    // 解密单个分片；认证失败（篡改、密钥错、分片序号错）返回空并清零已产出明文
    static QByteArray decryptChunk(const QByteArray &key, const QByteArray &iv,
                                   int chunkIndex, const QByteArray &ciphertext);

    // 一次性计算 SHA-256 并返回小写 hex（大文件请改用 Sha256Stream）
    static QString sha256Hex(const QByteArray &data);

    // 流式 SHA-256：分片上传/组装时逐片累加，避免把整个文件读进内存
    class Sha256Stream
    {
    public:
        void addData(const QByteArray &data);
        // 结束摘要（小写 hex）。调用后不应再 addData
        QString hexDigest() const;

    private:
        QCryptographicHash m_hash{QCryptographicHash::Sha256};
    };

    // 票据（上传/下载授权凭据）
    // 票据为高熵随机串，明文只在签发响应中返回一次；服务端与 HTTP 数据面只保存
    // 并比对 SHA-256 摘要（与 session token 同一套做法，库泄露不等于凭据泄露）
    static QString generateTicket();
    // 票据摘要（小写 hex）；空票据返回空串
    static QString ticketHash(const QString &ticket);
};

} // namespace XYChat::Security
