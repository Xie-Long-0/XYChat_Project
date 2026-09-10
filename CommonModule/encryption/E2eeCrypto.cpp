#include "E2eeCrypto.h"

#include <QJsonArray>
#include <QJsonDocument>

#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include "security/SecureMemory.h"

namespace XYChat::Security
{

namespace
{
constexpr int X25519KeySize = 32;
constexpr int GcmIvSize = 12;
constexpr int GcmTagSize = 16;
constexpr int EnvelopeVersion = 1;
const QByteArray HkdfSalt = QByteArrayLiteral("xychat-e2ee-v1");

// 从原始 32 字节私钥构建 X25519 EVP_PKEY
EVP_PKEY *pkeyFromRawPrivate(const QByteArray &privateKey)
{
    if (privateKey.size() != X25519KeySize) {
        return nullptr;
    }
    return EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr,
                                        reinterpret_cast<const unsigned char *>(privateKey.constData()),
                                        X25519KeySize);
}

// 从原始 32 字节公钥构建 X25519 EVP_PKEY
EVP_PKEY *pkeyFromRawPublic(const QByteArray &publicKey)
{
    if (publicKey.size() != X25519KeySize) {
        return nullptr;
    }
    return EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr,
                                       reinterpret_cast<const unsigned char *>(publicKey.constData()),
                                       X25519KeySize);
}

QString toBase64(const QByteArray &data)
{
    return QString::fromLatin1(data.toBase64());
}

QByteArray fromBase64(const QString &data, bool *ok)
{
    const QByteArray decoded = QByteArray::fromBase64(data.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (ok) {
        *ok = !decoded.isEmpty() || data.isEmpty();
    }
    return decoded;
}
} // namespace

// 密钥操作

E2eeCrypto::KeyPair E2eeCrypto::generateX25519KeyPair()
{
    KeyPair result;

    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
    if (!ctx) {
        return result;
    }

    EVP_PKEY *pkey = nullptr;
    bool ok = EVP_PKEY_keygen_init(ctx) > 0 && EVP_PKEY_keygen(ctx, &pkey) > 0;
    EVP_PKEY_CTX_free(ctx);
    if (!ok || !pkey) {
        if (pkey) {
            EVP_PKEY_free(pkey);
        }
        return result;
    }

    size_t pubLen = X25519KeySize;
    size_t privLen = X25519KeySize;
    result.publicKey.resize(X25519KeySize);
    result.privateKey.resize(X25519KeySize);
    ok = EVP_PKEY_get_raw_public_key(pkey,
                                     reinterpret_cast<unsigned char *>(result.publicKey.data()), &pubLen) > 0
        && EVP_PKEY_get_raw_private_key(pkey,
                                        reinterpret_cast<unsigned char *>(result.privateKey.data()), &privLen) > 0;
    EVP_PKEY_free(pkey);

    if (!ok || pubLen != X25519KeySize || privLen != X25519KeySize) {
        SecureMemory::wipe(result.privateKey);
        return KeyPair{};
    }

    result.valid = true;
    return result;
}

E2eeCrypto::KeyPair E2eeCrypto::keyPairFromPrivateKey(const QByteArray &privateKey)
{
    KeyPair result;

    EVP_PKEY *pkey = pkeyFromRawPrivate(privateKey);
    if (!pkey) {
        return result;
    }

    size_t pubLen = X25519KeySize;
    result.publicKey.resize(X25519KeySize);
    const bool ok = EVP_PKEY_get_raw_public_key(pkey,
                                                reinterpret_cast<unsigned char *>(result.publicKey.data()),
                                                &pubLen) > 0;
    EVP_PKEY_free(pkey);

    if (!ok || pubLen != X25519KeySize) {
        return KeyPair{};
    }

    result.privateKey = privateKey;
    result.valid = true;
    return result;
}

QByteArray E2eeCrypto::generateRandomBytes(int length)
{
    if (length <= 0) {
        return {};
    }
    QByteArray out(length, Qt::Uninitialized);
    if (RAND_bytes(reinterpret_cast<unsigned char *>(out.data()), length) != 1) {
        return {};
    }
    return out;
}

QByteArray E2eeCrypto::ecdh(const QByteArray &privateKey, const QByteArray &peerPublicKey)
{
    EVP_PKEY *priv = pkeyFromRawPrivate(privateKey);
    EVP_PKEY *peer = pkeyFromRawPublic(peerPublicKey);
    if (!priv || !peer) {
        if (priv) EVP_PKEY_free(priv);
        if (peer) EVP_PKEY_free(peer);
        return {};
    }

    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(priv, nullptr);
    QByteArray shared;
    if (ctx && EVP_PKEY_derive_init(ctx) > 0 && EVP_PKEY_derive_set_peer(ctx, peer) > 0) {
        size_t outLen = 0;
        if (EVP_PKEY_derive(ctx, nullptr, &outLen) > 0 && outLen == X25519KeySize) {
            shared.resize(static_cast<int>(outLen));
            if (EVP_PKEY_derive(ctx, reinterpret_cast<unsigned char *>(shared.data()), &outLen) <= 0) {
                SecureMemory::wipe(shared);
            }
        }
    }

    if (ctx) EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(priv);
    EVP_PKEY_free(peer);
    return shared;
}

QByteArray E2eeCrypto::deriveMessageKey(const QByteArray &sharedSecret)
{
    if (sharedSecret.isEmpty()) {
        return {};
    }

    QByteArray derived;
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
    if (!ctx) {
        return derived;
    }

    size_t outLen = 32;
    derived.resize(32);
    bool ok = EVP_PKEY_derive_init(ctx) > 0
        && EVP_PKEY_CTX_set_hkdf_md(ctx, EVP_sha256()) > 0
        && EVP_PKEY_CTX_set1_hkdf_salt(ctx,
                                       reinterpret_cast<const unsigned char *>(HkdfSalt.constData()),
                                       HkdfSalt.size()) > 0
        && EVP_PKEY_CTX_set1_hkdf_key(ctx,
                                      reinterpret_cast<const unsigned char *>(sharedSecret.constData()),
                                      sharedSecret.size()) > 0
        && EVP_PKEY_derive(ctx, reinterpret_cast<unsigned char *>(derived.data()), &outLen) > 0
        && outLen == 32;

    EVP_PKEY_CTX_free(ctx);
    if (!ok) {
        SecureMemory::wipe(derived);
        return {};
    }
    return derived;
}

QString E2eeCrypto::publicKeyFingerprint(const QByteArray &publicKey)
{
    if (publicKey.isEmpty()) {
        return {};
    }
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char *>(publicKey.constData()),
           publicKey.size(), hash);
    return QString::fromLatin1(QByteArray(reinterpret_cast<char *>(hash), 16).toHex());
}

// 消息加解密

E2eeCrypto::GcmResult E2eeCrypto::aesGcmEncrypt(const QByteArray &key, const QByteArray &plaintext)
{
    GcmResult result;
    if (key.size() != 32) {
        return result;
    }

    result.iv.resize(GcmIvSize);
    if (RAND_bytes(reinterpret_cast<unsigned char *>(result.iv.data()), GcmIvSize) != 1) {
        return GcmResult{};
    }

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        return GcmResult{};
    }

    bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) > 0
        && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, GcmIvSize, nullptr) > 0
        && EVP_EncryptInit_ex(ctx, nullptr, nullptr,
                              reinterpret_cast<const unsigned char *>(key.constData()),
                              reinterpret_cast<const unsigned char *>(result.iv.constData())) > 0;

    if (ok) {
        // 密文 + 认证标签
        result.ciphertext.resize(plaintext.size() + GcmTagSize);
        int outLen = 0;
        ok = EVP_EncryptUpdate(ctx,
                               reinterpret_cast<unsigned char *>(result.ciphertext.data()), &outLen,
                               reinterpret_cast<const unsigned char *>(plaintext.constData()),
                               plaintext.size()) > 0;
        int totalLen = outLen;
        ok = ok && EVP_EncryptFinal_ex(ctx,
                                       reinterpret_cast<unsigned char *>(result.ciphertext.data()) + totalLen,
                                       &outLen) > 0;
        totalLen += outLen;
        // GCM 标签追加在密文末尾
        ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, GcmTagSize,
                                       reinterpret_cast<unsigned char *>(result.ciphertext.data()) + totalLen) > 0;
        result.ciphertext.resize(totalLen + GcmTagSize);
    }

    EVP_CIPHER_CTX_free(ctx);
    if (!ok) {
        return GcmResult{};
    }
    result.valid = true;
    return result;
}

QByteArray E2eeCrypto::aesGcmDecrypt(const QByteArray &key, const QByteArray &iv,
                                     const QByteArray &ciphertext)
{
    if (key.size() != 32 || iv.size() != GcmIvSize || ciphertext.size() < GcmTagSize) {
        return {};
    }

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        return {};
    }

    QByteArray plaintext;
    bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) > 0
        && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, GcmIvSize, nullptr) > 0
        && EVP_DecryptInit_ex(ctx, nullptr, nullptr,
                              reinterpret_cast<const unsigned char *>(key.constData()),
                              reinterpret_cast<const unsigned char *>(iv.constData())) > 0;

    if (ok) {
        const int cipherLen = ciphertext.size() - GcmTagSize;
        plaintext.resize(cipherLen);
        int outLen = 0;
        ok = EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char *>(plaintext.data()), &outLen,
                               reinterpret_cast<const unsigned char *>(ciphertext.constData()),
                               cipherLen) > 0;
        plaintext.resize(outLen);
        // 设置认证标签后再 Final（标签失败则明文被丢弃）
        ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, GcmTagSize,
                                       const_cast<char *>(ciphertext.constData()) + cipherLen) > 0;
        int finalLen = 0;
        QByteArray finalBuf(GcmTagSize, Qt::Uninitialized);
        if (!ok || EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char *>(finalBuf.data()), &finalLen) <= 0) {
            ok = false; // 认证失败：篡改或密钥错误
        } else if (finalLen > 0) {
            plaintext.append(finalBuf.left(finalLen));
        }
    }

    EVP_CIPHER_CTX_free(ctx);
    if (!ok) {
        SecureMemory::wipe(plaintext);
        return {};
    }
    return plaintext;
}

// M8: 带调用方 nonce 与 AAD 的 AES-256-GCM（文件分片加解密）

QByteArray E2eeCrypto::aesGcmEncryptAad(const QByteArray &key, const QByteArray &nonce,
                                        const QByteArray &plaintext, const QByteArray &aad)
{
    if (key.size() != 32 || nonce.size() != GcmIvSize) {
        return {};
    }

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        return {};
    }

    bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) > 0
        && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, GcmIvSize, nullptr) > 0
        && EVP_EncryptInit_ex(ctx, nullptr, nullptr,
                              reinterpret_cast<const unsigned char *>(key.constData()),
                              reinterpret_cast<const unsigned char *>(nonce.constData())) > 0;

    // AAD 先于明文送入且不产出密文字节（输出缓冲传 nullptr）
    if (ok && !aad.isEmpty()) {
        int aadLen = 0;
        ok = EVP_EncryptUpdate(ctx, nullptr, &aadLen,
                               reinterpret_cast<const unsigned char *>(aad.constData()),
                               aad.size()) > 0;
    }

    QByteArray out;
    if (ok) {
        out.resize(plaintext.size() + GcmTagSize);
        int outLen = 0;
        ok = EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char *>(out.data()), &outLen,
                               reinterpret_cast<const unsigned char *>(plaintext.constData()),
                               plaintext.size()) > 0;
        int totalLen = outLen;
        ok = ok && EVP_EncryptFinal_ex(ctx,
                                       reinterpret_cast<unsigned char *>(out.data()) + totalLen,
                                       &outLen) > 0;
        totalLen += outLen;
        ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, GcmTagSize,
                                       reinterpret_cast<unsigned char *>(out.data()) + totalLen) > 0;
        out.resize(totalLen + GcmTagSize);
    }

    EVP_CIPHER_CTX_free(ctx);
    if (!ok) {
        return {};
    }
    return out;
}

QByteArray E2eeCrypto::aesGcmDecryptAad(const QByteArray &key, const QByteArray &nonce,
                                        const QByteArray &ciphertext, const QByteArray &aad)
{
    if (key.size() != 32 || nonce.size() != GcmIvSize || ciphertext.size() < GcmTagSize) {
        return {};
    }

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        return {};
    }

    QByteArray plaintext;
    bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) > 0
        && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, GcmIvSize, nullptr) > 0
        && EVP_DecryptInit_ex(ctx, nullptr, nullptr,
                              reinterpret_cast<const unsigned char *>(key.constData()),
                              reinterpret_cast<const unsigned char *>(nonce.constData())) > 0;

    if (ok && !aad.isEmpty()) {
        int aadLen = 0;
        ok = EVP_DecryptUpdate(ctx, nullptr, &aadLen,
                               reinterpret_cast<const unsigned char *>(aad.constData()),
                               aad.size()) > 0;
    }

    if (ok) {
        const int cipherLen = ciphertext.size() - GcmTagSize;
        plaintext.resize(cipherLen);
        int outLen = 0;
        ok = EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char *>(plaintext.data()), &outLen,
                               reinterpret_cast<const unsigned char *>(ciphertext.constData()),
                               cipherLen) > 0;
        plaintext.resize(outLen);
        // 设置认证标签后再 Final：AAD 或分片序号不符时标签校验失败，明文被丢弃
        ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, GcmTagSize,
                                       const_cast<char *>(ciphertext.constData()) + cipherLen) > 0;
        int finalLen = 0;
        QByteArray finalBuf(GcmTagSize, Qt::Uninitialized);
        if (!ok || EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char *>(finalBuf.data()), &finalLen) <= 0) {
            ok = false; // 认证失败：篡改、密钥错误或 AAD（分片序号）不匹配
        } else if (finalLen > 0) {
            plaintext.append(finalBuf.left(finalLen));
        }
    }

    EVP_CIPHER_CTX_free(ctx);
    if (!ok) {
        SecureMemory::wipe(plaintext);
        return {};
    }
    return plaintext;
}

// envelope 编解码

QJsonObject E2eeCrypto::encodeEnvelope(const QList<EnvelopeEntry> &entries)
{
    QJsonArray devices;
    for (const EnvelopeEntry &entry : entries) {
        QJsonObject obj;
        obj["deviceId"] = entry.deviceId;
        obj["prekeyId"] = entry.prekeyId;
        obj["eph"] = toBase64(entry.ephemeralPublicKey);
        obj["iv"] = toBase64(entry.iv);
        obj["ct"] = toBase64(entry.ciphertext);
        devices.append(obj);
    }

    QJsonObject root;
    root["v"] = EnvelopeVersion;
    root["devices"] = devices;
    return root;
}

QList<E2eeCrypto::EnvelopeEntry> E2eeCrypto::decodeEnvelope(const QString &content, bool *ok)
{
    if (ok) {
        *ok = false;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(content.toUtf8());
    if (!doc.isObject()) {
        return {};
    }
    const QJsonObject root = doc.object();
    if (root["v"].toInt(-1) != EnvelopeVersion || !root["devices"].isArray()) {
        return {};
    }

    QList<EnvelopeEntry> entries;
    const QJsonArray devices = root["devices"].toArray();
    if (devices.isEmpty()) {
        return {};
    }

    for (const QJsonValue &value : devices) {
        if (!value.isObject()) {
            return {};
        }
        const QJsonObject obj = value.toObject();

        EnvelopeEntry entry;
        entry.deviceId = obj["deviceId"].toString();
        entry.prekeyId = static_cast<qint64>(obj["prekeyId"].toDouble());
        entry.ephemeralPublicKey = fromBase64(obj["eph"].toString(), nullptr);
        entry.iv = fromBase64(obj["iv"].toString(), nullptr);
        entry.ciphertext = fromBase64(obj["ct"].toString(), nullptr);

        if (entry.deviceId.isEmpty() || entry.prekeyId < 0
            || entry.ephemeralPublicKey.size() != X25519KeySize
            || entry.iv.size() != GcmIvSize
            || entry.ciphertext.size() < GcmTagSize) {
            return {};
        }
        entries.append(entry);
    }

    if (ok) {
        *ok = true;
    }
    return entries;
}

bool E2eeCrypto::looksLikeEnvelope(const QString &content)
{
    if (content.isEmpty() || content[0] != QLatin1Char('{')) {
        return false;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(content.toUtf8());
    return doc.isObject() && doc.object()["v"].toInt(-1) == EnvelopeVersion
        && doc.object().contains("devices");
}

} // namespace XYChat::Security
