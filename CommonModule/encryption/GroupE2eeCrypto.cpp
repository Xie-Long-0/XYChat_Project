#include "GroupE2eeCrypto.h"

#include <QJsonArray>
#include <QJsonDocument>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include "security/SecureMemory.h"

namespace XYChat::Security
{

namespace
{
constexpr int Ed25519KeySize = 32;
constexpr int Ed25519SigSize = 64;
constexpr int ChainKeySize = 32;
constexpr int GcmIvSize = 12;
constexpr int GcmTagSize = 16;
constexpr int DistributionVersion = 1;
constexpr int GroupMessageVersion = 1;
const QByteArray ChainHkdfSalt = QByteArrayLiteral("xychat-grp-chain");
const QByteArray MessageHkdfInfo = QByteArrayLiteral("xychat-grp-msg");

QString toBase64(const QByteArray &data)
{
    return QString::fromLatin1(data.toBase64());
}

QByteArray fromBase64(const QString &data, bool *ok)
{
    const QByteArray decoded = QByteArray::fromBase64(data.toLatin1(),
                                                      QByteArray::AbortOnBase64DecodingErrors);
    if (ok) {
        *ok = !decoded.isEmpty() || data.isEmpty();
    }
    return decoded;
}

QByteArray hkdfSingle(const QByteArray &key, const QByteArray &salt, const QByteArray &info)
{
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
                                       reinterpret_cast<const unsigned char *>(salt.constData()),
                                       salt.size()) > 0
        && EVP_PKEY_CTX_set1_hkdf_key(ctx,
                                      reinterpret_cast<const unsigned char *>(key.constData()),
                                      key.size()) > 0
        && (!info.isEmpty()
            ? EVP_PKEY_CTX_add1_hkdf_info(ctx,
                                          reinterpret_cast<const unsigned char *>(info.constData()),
                                          info.size()) > 0
            : true)
        && EVP_PKEY_derive(ctx, reinterpret_cast<unsigned char *>(derived.data()), &outLen) > 0
        && outLen == 32;

    EVP_PKEY_CTX_free(ctx);
    if (!ok) {
        SecureMemory::wipe(derived);
        return {};
    }
    return derived;
}

EVP_PKEY *edPrivateKey(const QByteArray &privateKey)
{
    if (privateKey.size() != Ed25519KeySize) {
        return nullptr;
    }
    return EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
                                        reinterpret_cast<const unsigned char *>(privateKey.constData()),
                                        Ed25519KeySize);
}

EVP_PKEY *edPublicKey(const QByteArray &publicKey)
{
    if (publicKey.size() != Ed25519KeySize) {
        return nullptr;
    }
    return EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
                                       reinterpret_cast<const unsigned char *>(publicKey.constData()),
                                       Ed25519KeySize);
}
} // namespace

GroupE2eeCrypto::SenderKey GroupE2eeCrypto::generateSenderKey()
{
    SenderKey key;

    EVP_PKEY *pkey = EVP_PKEY_Q_keygen(nullptr, nullptr, "ED25519");
    if (!pkey) {
        return key;
    }

    key.publicSigningKey.resize(Ed25519KeySize);
    key.privateSigningKey.resize(Ed25519KeySize);
    size_t pubLen = Ed25519KeySize;
    size_t privLen = Ed25519KeySize;
    bool ok = EVP_PKEY_get_raw_public_key(pkey,
                                          reinterpret_cast<unsigned char *>(key.publicSigningKey.data()),
                                          &pubLen) > 0
        && EVP_PKEY_get_raw_private_key(pkey,
                                        reinterpret_cast<unsigned char *>(key.privateSigningKey.data()),
                                        &privLen) > 0;
    EVP_PKEY_free(pkey);

    if (!ok || pubLen != Ed25519KeySize || privLen != Ed25519KeySize) {
        SecureMemory::wipe(key.privateSigningKey);
        return SenderKey{};
    }

    key.chainKey = E2eeCrypto::generateRandomBytes(ChainKeySize);
    if (key.chainKey.size() != ChainKeySize) {
        SecureMemory::wipe(key.privateSigningKey);
        SecureMemory::wipe(key.chainKey);
        return SenderKey{};
    }

    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char *>(key.publicSigningKey.constData()),
           key.publicSigningKey.size(), hash);
    key.keyId = QString::fromLatin1(QByteArray(reinterpret_cast<char *>(hash), 16).toHex());
    key.iteration = 0;
    key.valid = true;
    return key;
}

QByteArray GroupE2eeCrypto::ratchetChainKey(const QByteArray &chainKey)
{
    if (chainKey.size() != ChainKeySize) {
        return {};
    }
    return hkdfSingle(chainKey, ChainHkdfSalt, QByteArray());
}

QByteArray GroupE2eeCrypto::deriveMessageKey(const QByteArray &chainKey, int iteration)
{
    if (chainKey.size() != ChainKeySize || iteration < 0) {
        return {};
    }
    const QByteArray info = MessageHkdfInfo + QByteArray::number(iteration);
    return hkdfSingle(chainKey, ChainHkdfSalt, info);
}

QByteArray GroupE2eeCrypto::sign(const QByteArray &privateSigningKey,
                                 const QByteArray &message)
{
    if (privateSigningKey.size() != Ed25519KeySize || message.isEmpty()) {
        return {};
    }

    EVP_PKEY *pkey = edPrivateKey(privateSigningKey);
    if (!pkey) {
        return {};
    }

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    QByteArray signature;
    bool ok = ctx != nullptr
        && EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, pkey) > 0;

    size_t sigLen = Ed25519SigSize;
    signature.resize(Ed25519SigSize);
    ok = ok
        && EVP_DigestSign(ctx,
                          reinterpret_cast<unsigned char *>(signature.data()),
                          &sigLen,
                          reinterpret_cast<const unsigned char *>(message.constData()),
                          message.size()) > 0
        && sigLen == Ed25519SigSize;

    if (ctx) EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);

    if (!ok) {
        return {};
    }
    return signature;
}

bool GroupE2eeCrypto::verify(const QByteArray &publicSigningKey,
                             const QByteArray &message,
                             const QByteArray &signature)
{
    if (publicSigningKey.size() != Ed25519KeySize
        || signature.size() != Ed25519SigSize
        || message.isEmpty()) {
        return false;
    }

    EVP_PKEY *pkey = edPublicKey(publicSigningKey);
    if (!pkey) {
        return false;
    }

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    bool ok = ctx != nullptr
        && EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, pkey) > 0
        && EVP_DigestVerify(ctx,
                            reinterpret_cast<const unsigned char *>(signature.constData()),
                            signature.size(),
                            reinterpret_cast<const unsigned char *>(message.constData()),
                            message.size()) > 0;

    if (ctx) EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return ok;
}

GroupE2eeCrypto::EncryptedMessage GroupE2eeCrypto::encryptMessage(SenderKey &key,
                                                                  const QByteArray &plaintext)
{
    EncryptedMessage result;
    if (!key.valid || plaintext.isEmpty()) {
        return result;
    }

    // ratchet 前进一步
    const QByteArray newChainKey = ratchetChainKey(key.chainKey);
    if (newChainKey.isEmpty()) {
        return result;
    }
    SecureMemory::wipe(key.chainKey);
    key.chainKey = newChainKey;
    key.iteration += 1;

    QByteArray messageKey = deriveMessageKey(key.chainKey, key.iteration);
    if (messageKey.isEmpty()) {
        return result;
    }

    const auto gcm = E2eeCrypto::aesGcmEncrypt(messageKey, plaintext);
    SecureMemory::wipe(messageKey);
    if (!gcm.valid) {
        return result;
    }

    const QByteArray signedPayload = gcm.iv + gcm.ciphertext;
    const QByteArray signature = sign(key.privateSigningKey, signedPayload);
    if (signature.isEmpty()) {
        return result;
    }

    result.keyId = key.keyId;
    result.iteration = key.iteration;
    result.iv = gcm.iv;
    result.ciphertext = gcm.ciphertext;
    result.signature = signature;
    result.valid = true;
    return result;
}

QByteArray GroupE2eeCrypto::decryptMessage(QByteArray &chainKey,
                                           int &iteration,
                                           const QByteArray &publicSigningKey,
                                           const EncryptedMessage &msg,
                                           QMap<int, QByteArray> *skippedKeys)
{
    if (!msg.valid
        || msg.iv.size() != GcmIvSize
        || msg.ciphertext.size() < GcmTagSize
        || msg.signature.size() != Ed25519SigSize
        || msg.iteration <= 0
        || chainKey.size() != ChainKeySize
        || iteration < 0) {
        return {};
    }

    // 乱序容忍：先查已跳过的消息密钥缓存。命中时不改动 chainKey/iteration
    // （该消息属于已被跨越的历史迭代）
    if (skippedKeys) {
        auto cached = skippedKeys->find(msg.iteration);
        if (cached != skippedKeys->end()) {
            // 先认证后消费：伪造密文/签名不得烧毁合法的跳序密钥（否则一条注入
            // 消息就能使随后到达的真实乱序消息永久不可解）。失败分支的 wipe 会
            // 因隐式共享而 detach，只清零本地副本，map 内条目不受影响
            const QByteArray cachedKey = cached.value();
            const QByteArray signedPayload = msg.iv + msg.ciphertext;
            QByteArray plaintext = E2eeCrypto::aesGcmDecrypt(cachedKey, msg.iv, msg.ciphertext);
            if (plaintext.isEmpty()) {
                SecureMemory::wipe(const_cast<QByteArray &>(cachedKey));
                return {};
            }
            if (!verify(publicSigningKey, signedPayload, msg.signature)) {
                SecureMemory::wipe(plaintext);
                SecureMemory::wipe(const_cast<QByteArray &>(cachedKey));
                return {};
            }
            // 认证成功：一次性消费该条目防重放。先 erase 使 cachedKey 成为唯一持有者，
            // 再 wipe 才能真正清零底层缓冲（共享状态下 wipe 会 detach 到副本）
            skippedKeys->erase(cached);
            SecureMemory::wipe(const_cast<QByteArray &>(cachedKey));
            return plaintext;
        }
    }

    // 不允许回退（重放，或乱序且无缓存可用）
    if (msg.iteration <= iteration) {
        return {};
    }

    // DoS 防护：拒绝超大跳跃（msg.iteration 由对端控制，上限内才允许同步 ratchet）
    if (msg.iteration - iteration > MaxRatchetSteps) {
        return {};
    }

    // 将本地 chainKey ratchet 到消息迭代次数；途中派生的消息密钥先存 pending，
    // 待解密与验签全部通过后才提交（伪造输入不得污染链状态与密钥缓存）
    QByteArray currentChainKey = chainKey;
    int currentIteration = iteration;
    QMap<int, QByteArray> pending;
    auto discard = [&currentChainKey, &pending]() {
        for (auto it = pending.begin(); it != pending.end(); ++it) {
            SecureMemory::wipe(it.value());
        }
        pending.clear();
        SecureMemory::wipe(currentChainKey);
    };
    while (currentIteration < msg.iteration) {
        const QByteArray next = ratchetChainKey(currentChainKey);
        if (next.isEmpty()) {
            discard();
            return {};
        }
        currentChainKey = next;
        ++currentIteration;
        // 被跨越的迭代即“在途/乱序”消息的密钥，缓存以供后续低 iteration 消息解密
        if (currentIteration < msg.iteration && skippedKeys) {
            const QByteArray skippedKey = deriveMessageKey(currentChainKey, currentIteration);
            if (skippedKey.isEmpty()) {
                discard();
                return {};
            }
            pending.insert(currentIteration, skippedKey);
        }
    }

    QByteArray messageKey = deriveMessageKey(currentChainKey, currentIteration);
    if (messageKey.isEmpty()) {
        discard();
        return {};
    }

    QByteArray plaintext = E2eeCrypto::aesGcmDecrypt(messageKey, msg.iv, msg.ciphertext);
    SecureMemory::wipe(messageKey);
    if (plaintext.isEmpty()) {
        discard();
        return {};
    }

    // 验证签名（签名在解密之后：伪造签名无法通过 GCM 认证）
    const QByteArray signedPayload = msg.iv + msg.ciphertext;
    if (!verify(publicSigningKey, signedPayload, msg.signature)) {
        SecureMemory::wipe(plaintext);
        discard();
        return {};
    }

    // 更新持久化状态
    SecureMemory::wipe(chainKey);
    chainKey = currentChainKey;
    iteration = currentIteration;
    if (skippedKeys) {
        for (auto it = pending.constBegin(); it != pending.constEnd(); ++it) {
            skippedKeys->insert(it.key(), it.value());
        }
        pending.clear();
        // 容量上限：超出时丢弃 iteration 最小的条目（最旧的在途消息）
        while (skippedKeys->size() > MaxSkippedMessageKeys) {
            auto oldest = skippedKeys->begin();
            SecureMemory::wipe(oldest.value());
            skippedKeys->erase(oldest);
        }
    }
    return plaintext;
}

namespace
{
// 单个设备条目的 JSON 形态
QJsonObject distributionDeviceObject(const GroupE2eeCrypto::DistributionEntry &entry)
{
    QJsonObject obj;
    obj["userId"] = entry.userId;
    obj["deviceId"] = entry.deviceId;
    obj["prekeyId"] = entry.envelope.prekeyId;
    obj["eph"] = toBase64(entry.envelope.ephemeralPublicKey);
    obj["iv"] = toBase64(entry.envelope.iv);
    obj["ct"] = toBase64(entry.envelope.ciphertext);
    return obj;
}

// 组装分发的根对象。抽出来供"整包编码"与"分片编码"共用，保证两者的字段布局
// 不会各自漂移（分片除了 devices 数组之外必须与整包逐字相同，接收端才能把它
// 当同一条分发处理）
QJsonObject encodeDistributionRoot(qint64 groupId, qint64 senderUserId,
                                   const QString &senderDeviceId,
                                   const GroupE2eeCrypto::SenderKey &key,
                                   const QJsonArray &devices)
{
    QJsonObject root;
    root["v"] = DistributionVersion;
    root["type"] = "sender_key_distribution";
    root["groupId"] = groupId;
    root["senderUserId"] = senderUserId;
    root["senderDeviceId"] = senderDeviceId;
    root["keyId"] = key.keyId;
    root["publicSigningKey"] = toBase64(key.publicSigningKey);
    root["devices"] = devices;
    return root;
}
} // namespace

QJsonObject GroupE2eeCrypto::encodeDistribution(qint64 groupId,
                                                qint64 senderUserId,
                                                const QString &senderDeviceId,
                                                const SenderKey &key,
                                                const QList<DistributionEntry> &entries)
{
    QJsonArray devices;
    for (const DistributionEntry &entry : entries) {
        devices.append(distributionDeviceObject(entry));
    }
    return encodeDistributionRoot(groupId, senderUserId, senderDeviceId, key, devices);
}

QList<QString> GroupE2eeCrypto::encodeDistributionChunks(qint64 groupId,
                                                         qint64 senderUserId,
                                                         const QString &senderDeviceId,
                                                         const SenderKey &key,
                                                         const QList<DistributionEntry> &entries,
                                                         int maxContentLength)
{
    QList<QString> chunks;
    if (groupId <= 0 || senderUserId <= 0 || senderDeviceId.isEmpty()
        || entries.isEmpty() || maxContentLength <= 0) {
        return chunks;
    }

    // 固定开销：与条目数无关的字段，外加空数组的 "[]"（2 个字符）。
    // Compact 输出无空白，故追加条目后的总长 = overhead + Σ(条目长度) + (条目数 - 1)
    const int overhead = QJsonDocument(encodeDistributionRoot(
        groupId, senderUserId, senderDeviceId, key, QJsonArray()))
        .toJson(QJsonDocument::Compact).size();
    if (overhead >= maxContentLength) {
        // 固定开销自身越界（keyId/deviceId 异常长）：无从切分
        return chunks;
    }

    QJsonArray current;
    int bodyLen = 0; // Σ(条目长度) + (条目数 - 1) 个逗号
    const auto flushChunk = [&]() {
        if (current.isEmpty()) {
            return;
        }
        chunks.append(QString::fromUtf8(QJsonDocument(encodeDistributionRoot(
            groupId, senderUserId, senderDeviceId, key, current))
            .toJson(QJsonDocument::Compact)));
        current = QJsonArray();
        bodyLen = 0;
    };

    for (const DistributionEntry &entry : entries) {
        const QJsonObject obj = distributionDeviceObject(entry);
        const int objLen = QJsonDocument(obj).toJson(QJsonDocument::Compact).size();
        const int cost = objLen + (current.isEmpty() ? 0 : 1);
        if (!current.isEmpty() && overhead + bodyLen + cost > maxContentLength) {
            flushChunk();
        }
        if (current.isEmpty()) {
            bodyLen = objLen;
        } else {
            bodyLen += 1 + objLen;
        }
        current.append(obj);
    }
    flushChunk();
    return chunks;
}

bool GroupE2eeCrypto::decodeDistribution(const QString &content,
                                         qint64 &groupId,
                                         qint64 &senderUserId,
                                         QString &senderDeviceId,
                                         SenderKey &key,
                                         QList<DistributionEntry> &entries)
{
    groupId = 0;
    senderUserId = 0;
    senderDeviceId.clear();
    key = SenderKey{};
    entries.clear();

    const QJsonDocument doc = QJsonDocument::fromJson(content.toUtf8());
    if (!doc.isObject()) {
        return false;
    }
    const QJsonObject root = doc.object();
    if (root["v"].toInt(-1) != DistributionVersion
        || root["type"].toString() != QLatin1String("sender_key_distribution")) {
        return false;
    }

    groupId = root["groupId"].toVariant().toLongLong();
    senderUserId = root["senderUserId"].toVariant().toLongLong();
    senderDeviceId = root["senderDeviceId"].toString();
    key.keyId = root["keyId"].toString();
    key.publicSigningKey = fromBase64(root["publicSigningKey"].toString(), nullptr);
    key.valid = !key.keyId.isEmpty() && key.publicSigningKey.size() == Ed25519KeySize;

    const QJsonArray devices = root["devices"].toArray();
    for (const QJsonValue &value : devices) {
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject obj = value.toObject();
        DistributionEntry entry;
        entry.userId = obj["userId"].toVariant().toLongLong();
        entry.deviceId = obj["deviceId"].toString();
        entry.envelope.deviceId = entry.deviceId;
        entry.envelope.prekeyId = static_cast<qint64>(obj["prekeyId"].toDouble());
        entry.envelope.ephemeralPublicKey = fromBase64(obj["eph"].toString(), nullptr);
        entry.envelope.iv = fromBase64(obj["iv"].toString(), nullptr);
        entry.envelope.ciphertext = fromBase64(obj["ct"].toString(), nullptr);
        if (entry.userId <= 0 || entry.deviceId.isEmpty() || entry.envelope.prekeyId < 0
            || entry.envelope.ephemeralPublicKey.size() != 32
            || entry.envelope.iv.size() != GcmIvSize
            || entry.envelope.ciphertext.size() < GcmTagSize) {
            continue;
        }
        entries.append(entry);
    }

    return key.valid;
}

bool GroupE2eeCrypto::looksLikeDistribution(const QString &content)
{
    if (content.isEmpty() || content[0] != QLatin1Char('{')) {
        return false;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(content.toUtf8());
    if (!doc.isObject()) {
        return false;
    }
    const QJsonObject root = doc.object();
    return root["v"].toInt(-1) == DistributionVersion
        && root["type"].toString() == QLatin1String("sender_key_distribution");
}

QJsonObject GroupE2eeCrypto::encodeGroupMessage(const EncryptedMessage &msg,
                                                const QString &senderDeviceId)
{
    QJsonObject root;
    root["v"] = GroupMessageVersion;
    root["type"] = "group_e2ee";
    root["keyId"] = msg.keyId;
    root["iteration"] = msg.iteration;
    root["senderDeviceId"] = senderDeviceId;
    root["iv"] = toBase64(msg.iv);
    root["ct"] = toBase64(msg.ciphertext);
    root["sig"] = toBase64(msg.signature);
    return root;
}

bool GroupE2eeCrypto::decodeGroupMessage(const QString &content, EncryptedMessage &msg,
                                         QString *senderDeviceId)
{
    msg = EncryptedMessage{};
    const QJsonDocument doc = QJsonDocument::fromJson(content.toUtf8());
    if (!doc.isObject()) {
        return false;
    }
    const QJsonObject root = doc.object();
    if (root["v"].toInt(-1) != GroupMessageVersion
        || root["type"].toString() != QLatin1String("group_e2ee")) {
        return false;
    }

    msg.keyId = root["keyId"].toString();
    msg.iteration = root["iteration"].toInt();
    if (senderDeviceId) {
        *senderDeviceId = root["senderDeviceId"].toString();
    }
    msg.iv = fromBase64(root["iv"].toString(), nullptr);
    msg.ciphertext = fromBase64(root["ct"].toString(), nullptr);
    msg.signature = fromBase64(root["sig"].toString(), nullptr);
    msg.valid = !msg.keyId.isEmpty()
        && msg.iteration > 0
        && msg.iteration <= MaxMessageIteration
        && msg.iv.size() == GcmIvSize
        && msg.ciphertext.size() >= GcmTagSize
        && msg.signature.size() == Ed25519SigSize;
    return msg.valid;
}

bool GroupE2eeCrypto::looksLikeGroupMessage(const QString &content)
{
    if (content.isEmpty() || content[0] != QLatin1Char('{')) {
        return false;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(content.toUtf8());
    if (!doc.isObject()) {
        return false;
    }
    const QJsonObject root = doc.object();
    return root["v"].toInt(-1) == GroupMessageVersion
        && root["type"].toString() == QLatin1String("group_e2ee");
}

} // namespace XYChat::Security
