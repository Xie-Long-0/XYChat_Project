#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>

#include "E2eeCrypto.h"

namespace XYChat::Security
{

/**
 * M7b: 群聊端到端加密（简化 Signal Sender Keys）
 *
 * - 每个发送方在每个群中独立生成 Sender Key：
 *   chainKey（32 字节随机种子） + Ed25519 签名密钥对。
 * - 群消息使用 chainKey ratchet 派生消息密钥，AES-256-GCM 加密，
 *   Ed25519 签名（iv || ciphertext）保证发送方身份可验证。
 * - chainKey 通过 pairwise E2EE（X25519 身份/预密钥）分发给每个群成员。
 * - 服务端只转发密文与分发消息，无法读取群消息正文。
 */
class GroupE2eeCrypto
{
public:
    // DoS 防护：单次解密允许的最大 ratchet 步数（参考 Signal skipped-key 上限），
    // 恶意成员发送超大 iteration 时直接拒绝，避免同步循环长时间占用线程
    static constexpr int MaxRatchetSteps = 2000;
    // 群消息 envelope 中 iteration 的绝对上界（解码侧纵深防御）
    static constexpr int MaxMessageIteration = 100000000;
    // 乱序容忍：单个 Sender Key 允许缓存的已跳过消息密钥条数上限。
    // 超出时丢弃 iteration 最小的条目，防止内存与本地落库无界增长
    static constexpr int MaxSkippedMessageKeys = 1000;

    struct SenderKey
    {
        QString keyId;            // 唯一标识：SHA-256(publicSigningKey).hex 前 32 字符
        QByteArray chainKey;      // 32 字节链式密钥种子（保密）
        QByteArray publicSigningKey;  // 32 字节 Ed25519 公钥
        QByteArray privateSigningKey; // 32 字节 Ed25519 私钥
        int iteration = 0;        // 当前已使用迭代次数
        bool valid = false;
    };

    // 生成新的 Sender Key
    static SenderKey generateSenderKey();

    // chainKey 前向 ratchet：HKDF-SHA256(chainKey, salt="xychat-grp-chain")
    static QByteArray ratchetChainKey(const QByteArray &chainKey);

    // 由 chainKey 与迭代次数派生 32 字节消息密钥
    static QByteArray deriveMessageKey(const QByteArray &chainKey, int iteration);

    // Ed25519 签名 / 验证（原始 32 字节公私钥）
    static QByteArray sign(const QByteArray &privateSigningKey,
                           const QByteArray &message);
    static bool verify(const QByteArray &publicSigningKey,
                       const QByteArray &message,
                       const QByteArray &signature);

    // 单条群消息加密结果
    struct EncryptedMessage
    {
        QString keyId;
        int iteration = 0;
        QByteArray iv;
        QByteArray ciphertext; // 密文 + GCM tag
        QByteArray signature;
        bool valid = false;
    };

    // 加密一条群消息（会修改 key.chainKey / key.iteration）
    static EncryptedMessage encryptMessage(SenderKey &key,
                                           const QByteArray &plaintext);

    // 解密群消息（chainKey 为接收方保存的链式密钥，publicSigningKey 为发送方公钥）
    // 成功返回明文；失败返回空；iteration 跳跃超过 MaxRatchetSteps 时拒绝（DoS 防护）
    //
    // skippedKeys 为可选的已跳过消息密钥缓存（iteration -> messageKey），用于容忍
    // 乱序投递：ratchet 前进时把途中派生的消息密钥写入缓存，之后到达的低 iteration
    // 消息仍可解密。群消息被编辑后会以新的 iteration 覆盖原正文，使 iteration 与
    // message_id 顺序解耦，按消息 id 升序批量解密（离线补收）时若没有该缓存，
    // 后到的低 iteration 消息会被回滚检查永久拒绝。
    // 缓存条目命中后即删除（一次性消费）；条数超过 MaxSkippedMessageKeys 时丢弃
    // iteration 最小者。仅在解密与验签全部通过后才提交状态与缓存（fail-closed）。
    // 传入 nullptr 时保持严格的“只前进不回退”语义。
    static QByteArray decryptMessage(QByteArray &chainKey,
                                     int &iteration,
                                     const QByteArray &publicSigningKey,
                                     const EncryptedMessage &msg,
                                     QMap<int, QByteArray> *skippedKeys = nullptr);

    // 分发消息：将 chainKey 用 pairwise E2EE 加密后发给每个目标设备
    struct DistributionEntry
    {
        qint64 userId = 0;
        QString deviceId;
        E2eeCrypto::EnvelopeEntry envelope; // 加密后的 chainKey
    };

    static QJsonObject encodeDistribution(qint64 groupId,
                                          qint64 senderUserId,
                                          const QString &senderDeviceId,
                                          const SenderKey &key,
                                          const QList<DistributionEntry> &entries);

    // 单条分发消息正文的长度预算。服务端以 RequestHandler::MaxGroupMessageLength
    // (16384) 校验群消息正文，超出即 InvalidRequest 且无重试价值；此处留约 8% 余量，
    // 因为长度按实际序列化结果度量（含 JSON 转义与 UTF-16 码元计数），
    // 而两侧常量分属不同模块、不能共享
    static constexpr int MaxDistributionContentLength = 15000;

    // 按长度预算把条目切成多条可分发的分片。每条分片携带**同一个** SenderKey
    // （同 keyId/publicSigningKey/chainKey），只覆盖其中一部分设备；接收端只在
    // 自己所属的那条分片里找条目即可解出 chainKey，故分片**无需重组**，且同一
    // keyId 被安装多次是幂等的。返回空表示无法编码（参数非法或固定开销即越界）。
    // 单条条目自身超预算时仍单独成片——宁可让整次分发失败（报错可见），
    // 也不能静默少发设备，后者会让发送方误以为分发成功而用他人无解的 key 加密
    static QList<QString> encodeDistributionChunks(qint64 groupId,
                                                   qint64 senderUserId,
                                                   const QString &senderDeviceId,
                                                   const SenderKey &key,
                                                   const QList<DistributionEntry> &entries,
                                                   int maxContentLength = MaxDistributionContentLength);

    static bool decodeDistribution(const QString &content,
                                   qint64 &groupId,
                                   qint64 &senderUserId,
                                   QString &senderDeviceId,
                                   SenderKey &key, // 仅填充 public 字段
                                   QList<DistributionEntry> &entries);

    static bool looksLikeDistribution(const QString &content);

    // 群消息 envelope 编解码（区别于 pairwise envelope）
    static QJsonObject encodeGroupMessage(const EncryptedMessage &msg,
                                          const QString &senderDeviceId = {});
    static bool decodeGroupMessage(const QString &content, EncryptedMessage &msg,
                                   QString *senderDeviceId = nullptr);
    static bool looksLikeGroupMessage(const QString &content);
};

} // namespace XYChat::Security
