#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace XYChat::Security
{

/**
 * M6: 端到端加密原语（简化 Signal 方案）
 *
 * - 密钥协商：X25519 ECDH
 *   shared = ECDH(eph_priv, peer_prekey_pub) || ECDH(eph_priv, peer_identity_pub)
 * - 密钥派生：HKDF-SHA256(shared, salt="xychat-e2ee-v1")
 * - 消息加密：AES-256-GCM（随机 12 字节 IV，认证标签防篡改）
 * - 所有密钥均为 32 字节原始格式，传输/存储用 Base64 编码
 */
class E2eeCrypto
{
public:
    struct KeyPair
    {
        QByteArray publicKey;  // 32 字节 X25519 原始公钥
        QByteArray privateKey; // 32 字节 X25519 原始私钥
        bool valid = false;
    };

    // 密钥操作
    // 生成 X25519 密钥对（身份密钥或一次性预密钥均可复用）
    static KeyPair generateX25519KeyPair();

    // 从原始私钥重建密钥对（公钥由私钥推导）
    static KeyPair keyPairFromPrivateKey(const QByteArray &privateKey);

    // 密码学安全随机字节（M6.5 本地存储密钥生成等用途），失败返回空
    static QByteArray generateRandomBytes(int length);

    // X25519 ECDH，输出 32 字节共享密钥；失败返回空
    static QByteArray ecdh(const QByteArray &privateKey, const QByteArray &peerPublicKey);

    // HKDF-SHA256 派生 32 字节消息密钥
    static QByteArray deriveMessageKey(const QByteArray &sharedSecret);

    // 身份公钥指纹（SHA-256 hex，前 16 字节），用于 TOFU 信任管理
    static QString publicKeyFingerprint(const QByteArray &publicKey);

    // 消息加解密
    struct GcmResult
    {
        QByteArray iv;         // 12 字节
        QByteArray ciphertext; // 密文 + GCM 认证标签
        bool valid = false;
    };

    static GcmResult aesGcmEncrypt(const QByteArray &key, const QByteArray &plaintext);
    static QByteArray aesGcmDecrypt(const QByteArray &key, const QByteArray &iv,
                                    const QByteArray &ciphertext);

    // M8: 带调用方指定 nonce 与 AAD 的 AES-256-GCM（文件分片加解密）
    //
    // 与 aesGcmEncrypt 的区别：nonce 不由本函数随机生成而由调用方派生，以便同一
    // 密钥下按分片序号确定性重建；AAD 参与认证但不加密，用于绑定分片位置
    // （防重排/截断）。输入输出格式同为 密文||16B 标签。
    // nonce 必须为 12 字节；密钥必须为 32 字节；失败（含认证失败）返回空
    static QByteArray aesGcmEncryptAad(const QByteArray &key, const QByteArray &nonce,
                                       const QByteArray &plaintext, const QByteArray &aad);
    static QByteArray aesGcmDecryptAad(const QByteArray &key, const QByteArray &nonce,
                                       const QByteArray &ciphertext, const QByteArray &aad);

    // envelope 编解码
    // envelope: {"v":1,"devices":[{"deviceId","prekeyId","eph","iv","ct"}...]}
    // prekeyId == SelfCopyPrekeyId 表示发送方自己设备的拷贝（仅用身份密钥加密，
    // 使发送方重新登录/多端同步后仍能解密自己发出的消息）
    static constexpr qint64 SelfCopyPrekeyId = 0;
    struct EnvelopeEntry
    {
        QString deviceId;
        qint64 prekeyId = 0;
        QByteArray ephemeralPublicKey; // 发送方临时公钥
        QByteArray iv;
        QByteArray ciphertext;
    };

    static QJsonObject encodeEnvelope(const QList<EnvelopeEntry> &entries);
    // 解析失败（格式非法/版本不支持）时返回空列表且 ok=false
    static QList<EnvelopeEntry> decodeEnvelope(const QString &content, bool *ok = nullptr);
    // 快速判断 content 是否为 v1 envelope（不要求完整字段）
    static bool looksLikeEnvelope(const QString &content);
};

} // namespace XYChat::Security
