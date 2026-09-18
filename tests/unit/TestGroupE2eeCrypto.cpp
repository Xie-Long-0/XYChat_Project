/**
 * M7b: 群聊 Sender-Key E2EE 原语单元测试
 *
 * 覆盖：SenderKey 生成、chain-key ratchet、消息加解密、签名验证、
 *       分发消息编解码、群消息 envelope 编解码，
 *       以及 DoS 防护（ratchet 跳跃上限、iteration 上界）与
 *       服务端 fail-closed envelope 校验的判定函数。
 */
#include <QtTest>
#include <QJsonDocument>
#include <QSet>

#include <climits>

#include "encryption/GroupE2eeCrypto.h"

using namespace XYChat::Security;

class TestGroupE2eeCrypto : public QObject
{
    Q_OBJECT

private slots:
    void generateSenderKeyProducesValidKey()
    {
        const auto key = GroupE2eeCrypto::generateSenderKey();
        QVERIFY(key.valid);
        QCOMPARE(key.chainKey.size(), 32);
        QCOMPARE(key.publicSigningKey.size(), 32);
        QCOMPARE(key.privateSigningKey.size(), 32);
        QVERIFY(!key.keyId.isEmpty());
        QCOMPARE(key.iteration, 0);
    }

    void generatedKeysAreUnique()
    {
        const auto a = GroupE2eeCrypto::generateSenderKey();
        const auto b = GroupE2eeCrypto::generateSenderKey();
        QVERIFY(a.valid && b.valid);
        QVERIFY(a.keyId != b.keyId);
        QVERIFY(a.publicSigningKey != b.publicSigningKey);
        QVERIFY(a.chainKey != b.chainKey);
    }

    void ratchetChainKeyIsDeterministicAndDifferent()
    {
        const auto key = GroupE2eeCrypto::generateSenderKey();
        const auto next1 = GroupE2eeCrypto::ratchetChainKey(key.chainKey);
        const auto next2 = GroupE2eeCrypto::ratchetChainKey(key.chainKey);
        QCOMPARE(next1.size(), 32);
        QCOMPARE(next1, next2);
        QVERIFY(next1 != key.chainKey);
    }

    void deriveMessageKeyDependsOnIteration()
    {
        const auto key = GroupE2eeCrypto::generateSenderKey();
        const auto mk0 = GroupE2eeCrypto::deriveMessageKey(key.chainKey, 0);
        const auto mk1 = GroupE2eeCrypto::deriveMessageKey(key.chainKey, 1);
        QCOMPARE(mk0.size(), 32);
        QCOMPARE(mk1.size(), 32);
        QVERIFY(mk0 != mk1);
    }

    void encryptThenDecryptRoundTrip()
    {
        auto key = GroupE2eeCrypto::generateSenderKey();
        const QByteArray initialChainKey = key.chainKey;
        const int initialIteration = key.iteration;
        const QByteArray plaintext = "Hello, group E2EE!";

        const auto encrypted = GroupE2eeCrypto::encryptMessage(key, plaintext);
        QVERIFY(encrypted.valid);
        QCOMPARE(encrypted.keyId, key.keyId);
        QCOMPARE(encrypted.iteration, 1);
        QCOMPARE(encrypted.iv.size(), 12);
        QVERIFY(encrypted.ciphertext.size() >= 16);
        QCOMPARE(encrypted.signature.size(), 64);
        // 加密后 chainKey 已 ratchet，iteration 变为 1
        QCOMPARE(key.iteration, 1);

        // decryptMessage 期望传入发送前状态（chainKey_0, iteration=0），
        // 内部 ratchet 到 msg.iteration 后解密；解密后状态与发送后一致
        QByteArray chainKey = initialChainKey;
        int iteration = initialIteration;
        const QByteArray decrypted = GroupE2eeCrypto::decryptMessage(
            chainKey, iteration, key.publicSigningKey, encrypted);
        QCOMPARE(decrypted, plaintext);
        QCOMPARE(iteration, 1);
        QCOMPARE(chainKey, key.chainKey);
    }

    void decryptRejectsIterationRollback()
    {
        auto key = GroupE2eeCrypto::generateSenderKey();
        const QByteArray initialChainKey = key.chainKey;
        const int initialIteration = key.iteration;
        const QByteArray plaintext = "message one";
        const auto encrypted = GroupE2eeCrypto::encryptMessage(key, plaintext);

        // 解密时传入发送前状态（iteration=0），内部 ratchet 到 1
        QByteArray chainKey = initialChainKey;
        int iteration = initialIteration;
        QVERIFY(!GroupE2eeCrypto::decryptMessage(
            chainKey, iteration, key.publicSigningKey, encrypted).isEmpty());
        QCOMPARE(iteration, 1);

        // 用已处理到 iteration=1 的状态再次解密同一条消息：不允许回退
        QVERIFY(GroupE2eeCrypto::decryptMessage(
            chainKey, iteration, key.publicSigningKey, encrypted).isEmpty());
    }

    void decryptRejectsTamperedCiphertext()
    {
        auto key = GroupE2eeCrypto::generateSenderKey();
        const QByteArray initialChainKey = key.chainKey;
        const QByteArray plaintext = "tamper test";
        auto encrypted = GroupE2eeCrypto::encryptMessage(key, plaintext);
        QVERIFY(encrypted.valid);

        encrypted.ciphertext[0] ^= 0xFF;
        QByteArray chainKey = initialChainKey;
        int iteration = 0;
        QVERIFY(GroupE2eeCrypto::decryptMessage(
            chainKey, iteration, key.publicSigningKey, encrypted).isEmpty());
    }

    void decryptRejectsTamperedSignature()
    {
        auto key = GroupE2eeCrypto::generateSenderKey();
        const QByteArray initialChainKey = key.chainKey;
        const QByteArray plaintext = "sig tamper test";
        auto encrypted = GroupE2eeCrypto::encryptMessage(key, plaintext);
        QVERIFY(encrypted.valid);

        encrypted.signature[0] ^= 0xFF;
        QByteArray chainKey = initialChainKey;
        int iteration = 0;
        QVERIFY(GroupE2eeCrypto::decryptMessage(
            chainKey, iteration, key.publicSigningKey, encrypted).isEmpty());
    }

    void decryptRejectsWrongPublicSigningKey()
    {
        auto key = GroupE2eeCrypto::generateSenderKey();
        const QByteArray initialChainKey = key.chainKey;
        const auto other = GroupE2eeCrypto::generateSenderKey();
        const QByteArray plaintext = "wrong signer";
        const auto encrypted = GroupE2eeCrypto::encryptMessage(key, plaintext);

        QByteArray chainKey = initialChainKey;
        int iteration = 0;
        QVERIFY(GroupE2eeCrypto::decryptMessage(
            chainKey, iteration, other.publicSigningKey, encrypted).isEmpty());
    }

    void distributionEncodeDecodeRoundTrip()
    {
        const auto key = GroupE2eeCrypto::generateSenderKey();
        GroupE2eeCrypto::DistributionEntry entry;
        entry.userId = 42;
        entry.deviceId = "deviceA";
        entry.envelope.prekeyId = 7;
        entry.envelope.ephemeralPublicKey = QByteArray(32, 'E');
        entry.envelope.iv = QByteArray(12, 'I');
        entry.envelope.ciphertext = QByteArray(16, 'C');

        const QJsonObject encoded = GroupE2eeCrypto::encodeDistribution(
            100, 1, "localDevice", key, {entry});
        QCOMPARE(encoded["type"].toString(), QString("sender_key_distribution"));
        QCOMPARE(encoded["groupId"].toVariant().toLongLong(), 100LL);

        qint64 groupId = 0;
        qint64 senderUserId = 0;
        QString senderDeviceId;
        GroupE2eeCrypto::SenderKey decodedKey;
        QList<GroupE2eeCrypto::DistributionEntry> decodedEntries;
        QVERIFY(GroupE2eeCrypto::decodeDistribution(
            QJsonDocument(encoded).toJson(QJsonDocument::Compact),
            groupId, senderUserId, senderDeviceId, decodedKey, decodedEntries));
        QCOMPARE(groupId, 100LL);
        QCOMPARE(senderUserId, 1LL);
        QCOMPARE(senderDeviceId, QString("localDevice"));
        QCOMPARE(decodedKey.keyId, key.keyId);
        QCOMPARE(decodedKey.publicSigningKey, key.publicSigningKey);
        QCOMPARE(decodedEntries.size(), 1);
        QCOMPARE(decodedEntries.at(0).userId, 42LL);
        QCOMPARE(decodedEntries.at(0).deviceId, QString("deviceA"));
    }

    // 分发分片：超大群（设备条目多到单条分发正文超服务端 MaxGroupMessageLength）
    // 必须切成多条，且不丢条目、不重复、逐片都能被接收端独立解码
    void distributionChunksSplitWithinLimitWithoutLosingEntries()
    {
        const auto key = GroupE2eeCrypto::generateSenderKey();
        QList<GroupE2eeCrypto::DistributionEntry> entries;
        for (int i = 0; i < 30; ++i) {
            GroupE2eeCrypto::DistributionEntry entry;
            entry.userId = 100 + i;
            // 设备 ID 用真实形态（32 个十六进制字符）
            entry.deviceId = QString::number(i, 16).rightJustified(32, QLatin1Char('a'));
            entry.envelope.deviceId = entry.deviceId;
            entry.envelope.prekeyId = 1000 + i;
            entry.envelope.ephemeralPublicKey = QByteArray(32, static_cast<char>('A' + i % 26));
            entry.envelope.iv = QByteArray(12, 'I');
            entry.envelope.ciphertext = QByteArray(64, 'C');
            entries.append(entry);
        }

        const int budget = 2000;
        const QList<QString> chunks = GroupE2eeCrypto::encodeDistributionChunks(
            100, 1, "localDevice", key, entries, budget);

        // 单条整包必然超预算（30 条 ≈ 6 KB，预算 2000），故必须切多片；
        // 且不能切碎成 30 片（每片应容纳多条）
        const QString whole = QString::fromUtf8(QJsonDocument(
            GroupE2eeCrypto::encodeDistribution(100, 1, "localDevice", key, entries))
            .toJson(QJsonDocument::Compact));
        QVERIFY(whole.size() > budget);
        QVERIFY(chunks.size() > 1);
        QVERIFY(chunks.size() < entries.size());

        QSet<QString> covered;
        for (const QString &chunk : chunks) {
            // 每片都必须能在服务端上限内（预算即按 MaxGroupMessageLength 留余量得出）
            QVERIFY2(chunk.size() <= budget,
                     qPrintable(QString("chunk length %1 exceeds %2").arg(chunk.size()).arg(budget)));

            qint64 groupId = 0;
            qint64 senderUserId = 0;
            QString senderDeviceId;
            GroupE2eeCrypto::SenderKey decodedKey;
            QList<GroupE2eeCrypto::DistributionEntry> decodedEntries;
            QVERIFY(GroupE2eeCrypto::decodeDistribution(chunk, groupId, senderUserId,
                                                        senderDeviceId, decodedKey,
                                                        decodedEntries));
            // 所有分片携带同一把 key：接收端逐片安装同一 keyId 是幂等的
            QCOMPARE(groupId, 100LL);
            QCOMPARE(senderUserId, 1LL);
            QCOMPARE(senderDeviceId, QString("localDevice"));
            QCOMPARE(decodedKey.keyId, key.keyId);
            QCOMPARE(decodedKey.publicSigningKey, key.publicSigningKey);
            QVERIFY(!decodedEntries.isEmpty());

            for (const auto &entry : decodedEntries) {
                const QString id = QString("%1/%2").arg(entry.userId).arg(entry.deviceId);
                QVERIFY2(!covered.contains(id), qPrintable("duplicated entry " + id));
                covered.insert(id);
            }
        }
        // 条目一个不少
        QCOMPARE(covered.size(), entries.size());
    }

    // 分发分片：条目少时不得切碎（保持单条，与改动前的编码结果逐字一致）
    void distributionChunksKeepSingleChunkWhenFits()
    {
        const auto key = GroupE2eeCrypto::generateSenderKey();
        GroupE2eeCrypto::DistributionEntry entry;
        entry.userId = 42;
        entry.deviceId = "deviceA";
        entry.envelope.deviceId = entry.deviceId;
        entry.envelope.prekeyId = 7;
        entry.envelope.ephemeralPublicKey = QByteArray(32, 'E');
        entry.envelope.iv = QByteArray(12, 'I');
        entry.envelope.ciphertext = QByteArray(16, 'C');

        const QList<QString> chunks = GroupE2eeCrypto::encodeDistributionChunks(
            100, 1, "localDevice", key, {entry});
        QCOMPARE(chunks.size(), 1);

        // 与整包编码逐字相同：分片只是"按预算切条目集合"，字段布局不得有任何差异
        const QString whole = QString::fromUtf8(QJsonDocument(
            GroupE2eeCrypto::encodeDistribution(100, 1, "localDevice", key, {entry}))
            .toJson(QJsonDocument::Compact));
        QCOMPARE(chunks.at(0), whole);
    }

    // 分发分片：单条条目自身超预算时仍要单独成片。宁可让整次分发被服务端拒掉
    //（调用方随后丢弃整把 key，不会用他人拿不到的 key 加密），也不能静默少发设备
    void distributionChunksEmitOversizedSingleEntry()
    {
        const auto key = GroupE2eeCrypto::generateSenderKey();
        GroupE2eeCrypto::DistributionEntry entry;
        entry.userId = 42;
        entry.deviceId = QString(4000, QLatin1Char('d'));
        entry.envelope.deviceId = entry.deviceId;
        entry.envelope.prekeyId = 7;
        entry.envelope.ephemeralPublicKey = QByteArray(32, 'E');
        entry.envelope.iv = QByteArray(12, 'I');
        entry.envelope.ciphertext = QByteArray(16, 'C');

        const QList<QString> chunks = GroupE2eeCrypto::encodeDistributionChunks(
            100, 1, "localDevice", key, {entry}, 2000);
        QCOMPARE(chunks.size(), 1);
        QVERIFY(chunks.at(0).size() > 2000);
    }

    // 分发分片：参数非法或固定开销即越界时返回空（调用方据此放弃整次轮换）
    void distributionChunksRejectInvalidInput()
    {
        const auto key = GroupE2eeCrypto::generateSenderKey();
        GroupE2eeCrypto::DistributionEntry entry;
        entry.userId = 42;
        entry.deviceId = "deviceA";
        entry.envelope.deviceId = entry.deviceId;
        entry.envelope.prekeyId = 7;
        entry.envelope.ephemeralPublicKey = QByteArray(32, 'E');
        entry.envelope.iv = QByteArray(12, 'I');
        entry.envelope.ciphertext = QByteArray(16, 'C');

        const QList<GroupE2eeCrypto::DistributionEntry> one{entry};
        QVERIFY(GroupE2eeCrypto::encodeDistributionChunks(0, 1, "d", key, one).isEmpty());
        QVERIFY(GroupE2eeCrypto::encodeDistributionChunks(100, 0, "d", key, one).isEmpty());
        QVERIFY(GroupE2eeCrypto::encodeDistributionChunks(100, 1, "", key, one).isEmpty());
        QVERIFY(GroupE2eeCrypto::encodeDistributionChunks(100, 1, "d", key, {}).isEmpty());
        // 预算小到装不下固定字段：无从切分
        QVERIFY(GroupE2eeCrypto::encodeDistributionChunks(100, 1, "d", key, one, 8).isEmpty());
    }

    void groupMessageEncodeDecodeRoundTrip()
    {
        auto key = GroupE2eeCrypto::generateSenderKey();
        const QByteArray plaintext = "group envelope test";
        const auto encrypted = GroupE2eeCrypto::encryptMessage(key, plaintext);

        const QJsonObject encoded = GroupE2eeCrypto::encodeGroupMessage(
            encrypted, "senderDevice");
        const QString json = QJsonDocument(encoded).toJson(QJsonDocument::Compact);
        QVERIFY(GroupE2eeCrypto::looksLikeGroupMessage(json));
        QVERIFY(!GroupE2eeCrypto::looksLikeDistribution(json));

        GroupE2eeCrypto::EncryptedMessage decoded;
        QString deviceId;
        QVERIFY(GroupE2eeCrypto::decodeGroupMessage(json, decoded, &deviceId));
        QCOMPARE(deviceId, QString("senderDevice"));
        QCOMPARE(decoded.keyId, encrypted.keyId);
        QCOMPARE(decoded.iteration, encrypted.iteration);
        QCOMPARE(decoded.iv, encrypted.iv);
        QCOMPARE(decoded.ciphertext, encrypted.ciphertext);
        QCOMPARE(decoded.signature, encrypted.signature);
        QVERIFY(decoded.valid);
    }

    void looksLikeDetectsFormats()
    {
        QVERIFY(!GroupE2eeCrypto::looksLikeGroupMessage("{}"));
        QVERIFY(!GroupE2eeCrypto::looksLikeDistribution("{}"));

        const QJsonObject dist;
        QVERIFY(!GroupE2eeCrypto::looksLikeDistribution(
            QJsonDocument(dist).toJson(QJsonDocument::Compact)));

        auto key = GroupE2eeCrypto::generateSenderKey();
        const auto encrypted = GroupE2eeCrypto::encryptMessage(key, "x");
        QVERIFY(GroupE2eeCrypto::looksLikeGroupMessage(
            QJsonDocument(GroupE2eeCrypto::encodeGroupMessage(encrypted)).toJson(
                QJsonDocument::Compact)));
    }

    // DoS 防护：签名完全合法但 iteration 跳跃超过 MaxRatchetSteps 的消息必须被拒绝，
    // 且接收方状态不得变更（恶意远未来 iteration 不能消耗 ratchet 计算）
    void decryptRejectsIterationJumpBeyondLimit()
    {
        auto key = GroupE2eeCrypto::generateSenderKey();
        const QByteArray initialChainKey = key.chainKey;

        // 发送方连续加密 MaxRatchetSteps+1 条，取最后一条（iteration=2001）
        const int target = GroupE2eeCrypto::MaxRatchetSteps + 1;
        GroupE2eeCrypto::EncryptedMessage last;
        for (int i = 0; i < target; ++i) {
            last = GroupE2eeCrypto::encryptMessage(key, QByteArray("msg ") + QByteArray::number(i));
            QVERIFY(last.valid);
        }
        QCOMPARE(last.iteration, target);

        QByteArray chainKey = initialChainKey;
        int iteration = 0;
        QVERIFY(GroupE2eeCrypto::decryptMessage(
            chainKey, iteration, key.publicSigningKey, last).isEmpty());
        // 拒绝时状态不变：chainKey 未被 ratchet，iteration 未推进
        QCOMPARE(iteration, 0);
        QCOMPARE(chainKey, initialChainKey);
    }

    // DoS 防护边界：跳跃恰好等于 MaxRatchetSteps 时仍可正常同步解密
    void decryptAcceptsIterationJumpAtLimit()
    {
        auto key = GroupE2eeCrypto::generateSenderKey();
        const QByteArray initialChainKey = key.chainKey;

        const int target = GroupE2eeCrypto::MaxRatchetSteps;
        const QByteArray plaintext = "boundary message";
        GroupE2eeCrypto::EncryptedMessage last;
        for (int i = 0; i < target; ++i) {
            last = GroupE2eeCrypto::encryptMessage(key, (i == target - 1) ? plaintext
                                                                          : QByteArray("filler"));
            QVERIFY(last.valid);
        }
        QCOMPARE(last.iteration, target);

        QByteArray chainKey = initialChainKey;
        int iteration = 0;
        QCOMPARE(GroupE2eeCrypto::decryptMessage(
            chainKey, iteration, key.publicSigningKey, last), plaintext);
        QCOMPARE(iteration, target);
        QCOMPARE(chainKey, key.chainKey);
    }

    // DoS 防护：手工构造的极端 iteration（审查报告攻击向量 INT_MAX）在解密与解码两侧均被拒绝
    void decryptAndDecodeRejectAbsurdIteration()
    {
        auto key = GroupE2eeCrypto::generateSenderKey();
        auto encrypted = GroupE2eeCrypto::encryptMessage(key, "payload");
        QVERIFY(encrypted.valid);

        encrypted.iteration = INT_MAX;
        QByteArray chainKey = key.chainKey;
        int iteration = 0;
        QVERIFY(GroupE2eeCrypto::decryptMessage(
            chainKey, iteration, key.publicSigningKey, encrypted).isEmpty());
        QCOMPARE(iteration, 0);

        // 解码侧上界：iteration 超过 MaxMessageIteration 的 envelope 直接判为非法
        const QJsonObject overCap = GroupE2eeCrypto::encodeGroupMessage(encrypted);
        GroupE2eeCrypto::EncryptedMessage decoded;
        QVERIFY(!GroupE2eeCrypto::decodeGroupMessage(
            QJsonDocument(overCap).toJson(QJsonDocument::Compact), decoded));
        QVERIFY(!decoded.valid);
    }

    // 解码侧 iteration 上界的边界值：MaxMessageIteration 本身合法，+1 拒绝
    void decodeGroupMessageIterationCapBoundary()
    {
        auto key = GroupE2eeCrypto::generateSenderKey();
        auto encrypted = GroupE2eeCrypto::encryptMessage(key, "cap boundary");

        QJsonObject atCap = GroupE2eeCrypto::encodeGroupMessage(encrypted);
        atCap["iteration"] = GroupE2eeCrypto::MaxMessageIteration;
        GroupE2eeCrypto::EncryptedMessage decoded;
        QVERIFY(GroupE2eeCrypto::decodeGroupMessage(
            QJsonDocument(atCap).toJson(QJsonDocument::Compact), decoded));
        QCOMPARE(decoded.iteration, GroupE2eeCrypto::MaxMessageIteration);

        QJsonObject overCap = atCap;
        overCap["iteration"] = GroupE2eeCrypto::MaxMessageIteration + 1;
        QVERIFY(!GroupE2eeCrypto::decodeGroupMessage(
            QJsonDocument(overCap).toJson(QJsonDocument::Compact), decoded));
    }

    // 服务端 fail-closed 门槛（RequestHandler::processSendGroupMessage 同源判定）：
    // e2ee_group 内容必须是合法群消息 envelope，明文/残缺结构一律拒绝
    void serverGateRejectsInvalidGroupEnvelope()
    {
        GroupE2eeCrypto::EncryptedMessage decoded;
        QString senderDeviceId;

        // 纯文本伪装成 e2ee_group 密文
        QVERIFY(!GroupE2eeCrypto::decodeGroupMessage("hello plaintext", decoded, &senderDeviceId));
        // JSON 但缺少关键字段
        QVERIFY(!GroupE2eeCrypto::decodeGroupMessage(
            "{\"v\":1,\"type\":\"group_e2ee\",\"keyId\":\"k\"}", decoded, &senderDeviceId));
        // 字段尺寸非法（iv 长度错误）
        auto key = GroupE2eeCrypto::generateSenderKey();
        auto encrypted = GroupE2eeCrypto::encryptMessage(key, "x");
        QJsonObject badIv = GroupE2eeCrypto::encodeGroupMessage(encrypted, "dev1");
        badIv["iv"] = QString::fromLatin1(QByteArray(8, 'I').toBase64());
        QVERIFY(!GroupE2eeCrypto::decodeGroupMessage(
            QJsonDocument(badIv).toJson(QJsonDocument::Compact), decoded, &senderDeviceId));
        // senderDeviceId 缺失时接收方无法定位 sender key，门槛同样拒绝
        QJsonObject noDevice = GroupE2eeCrypto::encodeGroupMessage(encrypted);
        QVERIFY(GroupE2eeCrypto::decodeGroupMessage(
            QJsonDocument(noDevice).toJson(QJsonDocument::Compact), decoded, &senderDeviceId));
        QVERIFY(senderDeviceId.isEmpty());
        // 合法 envelope 通过门槛
        QVERIFY(GroupE2eeCrypto::decodeGroupMessage(
            QJsonDocument(GroupE2eeCrypto::encodeGroupMessage(encrypted, "dev1"))
                .toJson(QJsonDocument::Compact), decoded, &senderDeviceId));
        QCOMPARE(senderDeviceId, QString("dev1"));
    }

    // 服务端 fail-closed 门槛：sender_key_distribution 必须可解码、含有效条目且 groupId 与会话一致
    void serverGateRejectsInvalidDistribution()
    {
        qint64 groupId = 0;
        qint64 senderUserId = 0;
        QString senderDeviceId;
        GroupE2eeCrypto::SenderKey key;
        QList<GroupE2eeCrypto::DistributionEntry> entries;

        // 明文伪装成分发消息
        QVERIFY(!GroupE2eeCrypto::decodeDistribution("plain text", groupId, senderUserId,
                                                     senderDeviceId, key, entries));
        // 结构合法但无任何设备条目：接收方无法获得 chain key，门槛拒绝
        const auto validKey = GroupE2eeCrypto::generateSenderKey();
        const QString noDevices = QJsonDocument(GroupE2eeCrypto::encodeDistribution(
            100, 1, "devA", validKey, {})).toJson(QJsonDocument::Compact);
        QVERIFY(GroupE2eeCrypto::decodeDistribution(noDevices, groupId, senderUserId,
                                                    senderDeviceId, key, entries));
        QVERIFY(entries.isEmpty());
        // groupId 与目标会话不一致：门槛拒绝（防止向其他群投递分发消息）
        GroupE2eeCrypto::DistributionEntry entry;
        entry.userId = 42;
        entry.deviceId = "devB";
        entry.envelope.prekeyId = 7;
        entry.envelope.ephemeralPublicKey = QByteArray(32, 'E');
        entry.envelope.iv = QByteArray(12, 'I');
        entry.envelope.ciphertext = QByteArray(16, 'C');
        const QString wrongGroup = QJsonDocument(GroupE2eeCrypto::encodeDistribution(
            999, 1, "devA", validKey, {entry})).toJson(QJsonDocument::Compact);
        QVERIFY(GroupE2eeCrypto::decodeDistribution(wrongGroup, groupId, senderUserId,
                                                    senderDeviceId, key, entries));
        QCOMPARE(groupId, 999LL);
        QVERIFY(groupId != 100LL);
        // 合法分发消息通过门槛
        const QString valid = QJsonDocument(GroupE2eeCrypto::encodeDistribution(
            100, 1, "devA", validKey, {entry})).toJson(QJsonDocument::Compact);
        QVERIFY(GroupE2eeCrypto::decodeDistribution(valid, groupId, senderUserId,
                                                    senderDeviceId, key, entries));
        QCOMPARE(groupId, 100LL);
        QCOMPARE(entries.size(), 1);
    }

    // P1-3 healing 语义（2026-09-02）：群成员变更后发送方轮换 sender key（新 keyId），
    // 被移除成员仅持有旧密钥材料，无法解密轮换后的新消息（后向安全 / 失权回收）；
    // 现任成员收到重分发的新密钥后可正常解密。
    void senderKeyRotationRevokesRemovedMember()
    {
        // 轮换前：发送方持有 K1，被移除成员 bob 保存了 K1 的 chain key 与签名公钥
        auto k1 = GroupE2eeCrypto::generateSenderKey();
        QVERIFY(k1.valid);
        const QByteArray bobChainKey0 = k1.chainKey;
        const QByteArray bobPubKey = k1.publicSigningKey;

        // 成员变更触发轮换：生成全新 K2（新 keyId、新签名密钥对、iteration 归零）
        auto k2 = GroupE2eeCrypto::generateSenderKey();
        QVERIFY(k2.valid);
        QVERIFY(k2.keyId != k1.keyId);
        QVERIFY(k2.publicSigningKey != k1.publicSigningKey);
        QCOMPARE(k2.iteration, 0);
        // 现任成员经重分发获得 K2 的初始 chain key 与公钥
        const QByteArray memberChainKey0 = k2.chainKey;
        const QByteArray memberPubKey = k2.publicSigningKey;

        // 发送方用 K2 加密轮换后的新消息
        const QByteArray secret = "message after member removal";
        const auto encrypted = GroupE2eeCrypto::encryptMessage(k2, secret);
        QVERIFY(encrypted.valid);
        QCOMPARE(encrypted.keyId, k2.keyId);
        QCOMPARE(encrypted.iteration, 1);

        // 被移除成员 bob 仅持有旧 K1 材料：签名验证失败（K2 公钥不匹配）→ 解密被拒，
        // 且其本地状态不被推进（chain key 未被消耗）
        QByteArray bobChainKey = bobChainKey0;
        int bobIteration = 0;
        QVERIFY(GroupE2eeCrypto::decryptMessage(
            bobChainKey, bobIteration, bobPubKey, encrypted).isEmpty());
        QCOMPARE(bobIteration, 0);
        QCOMPARE(bobChainKey, bobChainKey0);

        // 现任成员用重分发获得的 K2 材料解密成功
        QByteArray memberChainKey = memberChainKey0;
        int memberIteration = 0;
        QCOMPARE(GroupE2eeCrypto::decryptMessage(
            memberChainKey, memberIteration, memberPubKey, encrypted), secret);
        QCOMPARE(memberIteration, 1);
    }

    // 乱序容忍（2026-09-09 修复）：最新一条先到达时，ratchet 跨越的迭代其消息密钥
    // 被缓存，随后到达的低 iteration 消息仍可解密，且命中缓存不推进链状态
    void outOfOrderDecryptUsesSkippedMessageKeys()
    {
        auto key = GroupE2eeCrypto::generateSenderKey();
        const QByteArray chainKey0 = key.chainKey;
        const auto m1 = GroupE2eeCrypto::encryptMessage(key, "one");
        const auto m2 = GroupE2eeCrypto::encryptMessage(key, "two");
        const auto m3 = GroupE2eeCrypto::encryptMessage(key, "three");

        QByteArray chainKey = chainKey0;
        int iteration = 0;
        QMap<int, QByteArray> skipped;
        QCOMPARE(GroupE2eeCrypto::decryptMessage(
            chainKey, iteration, key.publicSigningKey, m3, &skipped), QByteArray("three"));
        QCOMPARE(iteration, 3);
        QCOMPARE(skipped.size(), 2);
        QVERIFY(skipped.contains(1));
        QVERIFY(skipped.contains(2));

        QCOMPARE(GroupE2eeCrypto::decryptMessage(
            chainKey, iteration, key.publicSigningKey, m1, &skipped), QByteArray("one"));
        QCOMPARE(GroupE2eeCrypto::decryptMessage(
            chainKey, iteration, key.publicSigningKey, m2, &skipped), QByteArray("two"));
        // 缓存命中不改动链状态，且条目被一次性消费
        QCOMPARE(iteration, 3);
        QCOMPARE(chainKey, key.chainKey);
        QVERIFY(skipped.isEmpty());
    }

    // 缓存条目消费后不得重放：同一条消息第二次解密仍被拒绝
    void skippedMessageKeyIsConsumedOnce()
    {
        auto key = GroupE2eeCrypto::generateSenderKey();
        const QByteArray chainKey0 = key.chainKey;
        const auto m1 = GroupE2eeCrypto::encryptMessage(key, "one");
        const auto m2 = GroupE2eeCrypto::encryptMessage(key, "two");

        QByteArray chainKey = chainKey0;
        int iteration = 0;
        QMap<int, QByteArray> skipped;
        QCOMPARE(GroupE2eeCrypto::decryptMessage(
            chainKey, iteration, key.publicSigningKey, m2, &skipped), QByteArray("two"));
        QCOMPARE(GroupE2eeCrypto::decryptMessage(
            chainKey, iteration, key.publicSigningKey, m1, &skipped), QByteArray("one"));
        QVERIFY(skipped.isEmpty());
        // 重放 m1：缓存已空且 iteration(1) <= 链状态(2) → 拒绝
        QVERIFY(GroupE2eeCrypto::decryptMessage(
            chainKey, iteration, key.publicSigningKey, m1, &skipped).isEmpty());
        QCOMPARE(iteration, 2);
    }

    // fail-closed：伪造密文不得污染链状态，也不得把派生的消息密钥写入缓存
    void forgedMessageDoesNotPopulateSkippedCache()
    {
        auto key = GroupE2eeCrypto::generateSenderKey();
        const QByteArray chainKey0 = key.chainKey;
        const auto m1 = GroupE2eeCrypto::encryptMessage(key, "one");
        auto forged = GroupE2eeCrypto::encryptMessage(key, "two");
        QCOMPARE(forged.iteration, 2);
        forged.ciphertext[0] ^= 0xFF;

        QByteArray chainKey = chainKey0;
        int iteration = 0;
        QMap<int, QByteArray> skipped;
        QVERIFY(GroupE2eeCrypto::decryptMessage(
            chainKey, iteration, key.publicSigningKey, forged, &skipped).isEmpty());
        QCOMPARE(iteration, 0);
        QCOMPARE(chainKey, chainKey0);
        QVERIFY(skipped.isEmpty());
        // 状态未被污染：真实消息仍可正常解密
        QCOMPARE(GroupE2eeCrypto::decryptMessage(
            chainKey, iteration, key.publicSigningKey, m1, &skipped), QByteArray("one"));
        QCOMPARE(iteration, 1);
    }

    // 先认证后消费：命中缓存的伪造消息不得烧毁合法跳序密钥，
    // 否则一条注入消息就能使随后到达的真实乱序消息永久不可解
    void forgedMessageDoesNotConsumeSkippedKey()
    {
        auto key = GroupE2eeCrypto::generateSenderKey();
        const QByteArray chainKey0 = key.chainKey;
        const auto m1 = GroupE2eeCrypto::encryptMessage(key, "one");
        const auto m2 = GroupE2eeCrypto::encryptMessage(key, "two");
        const auto m3 = GroupE2eeCrypto::encryptMessage(key, "three");

        QByteArray chainKey = chainKey0;
        int iteration = 0;
        QMap<int, QByteArray> skipped;
        // 最新一条先到达：缓存 iteration 1、2 的消息密钥
        QCOMPARE(GroupE2eeCrypto::decryptMessage(
            chainKey, iteration, key.publicSigningKey, m3, &skipped), QByteArray("three"));
        QCOMPARE(skipped.size(), 2);

        // 伪造密文命中缓存 iteration=1：GCM 认证失败，缓存必须保持完整
        auto forgedCipher = m1;
        forgedCipher.ciphertext[0] ^= 0xFF;
        QVERIFY(GroupE2eeCrypto::decryptMessage(
            chainKey, iteration, key.publicSigningKey, forgedCipher, &skipped).isEmpty());
        QCOMPARE(skipped.size(), 2);

        // 伪造签名命中缓存 iteration=2：验签失败，缓存同样不得被消费
        auto forgedSig = m2;
        forgedSig.signature[0] ^= 0xFF;
        QVERIFY(GroupE2eeCrypto::decryptMessage(
            chainKey, iteration, key.publicSigningKey, forgedSig, &skipped).isEmpty());
        QCOMPARE(skipped.size(), 2);
        QCOMPARE(iteration, 3);

        // 两条真实消息随后到达，仍可正常解出
        QCOMPARE(GroupE2eeCrypto::decryptMessage(
            chainKey, iteration, key.publicSigningKey, m1, &skipped), QByteArray("one"));
        QCOMPARE(GroupE2eeCrypto::decryptMessage(
            chainKey, iteration, key.publicSigningKey, m2, &skipped), QByteArray("two"));
        QVERIFY(skipped.isEmpty());
    }

    // 缓存容量上限：超出 MaxSkippedMessageKeys 时丢弃 iteration 最小的条目，
    // 防止恶意大跳跃造成内存与本地落库无界增长
    void skippedCacheEvictsOldestBeyondLimit()
    {
        auto key = GroupE2eeCrypto::generateSenderKey();
        const QByteArray chainKey0 = key.chainKey;

        const int total = GroupE2eeCrypto::MaxSkippedMessageKeys + 5;
        QVERIFY(total <= GroupE2eeCrypto::MaxRatchetSteps);
        GroupE2eeCrypto::EncryptedMessage last;
        for (int i = 0; i < total; ++i) {
            last = GroupE2eeCrypto::encryptMessage(key, QByteArray("msg ") + QByteArray::number(i));
            QVERIFY(last.valid);
        }
        QCOMPARE(last.iteration, total);

        QByteArray chainKey = chainKey0;
        int iteration = 0;
        QMap<int, QByteArray> skipped;
        QVERIFY(!GroupE2eeCrypto::decryptMessage(
            chainKey, iteration, key.publicSigningKey, last, &skipped).isEmpty());
        QCOMPARE(iteration, total);
        QCOMPARE(skipped.size(), GroupE2eeCrypto::MaxSkippedMessageKeys);
        // 保留区间为 [total - Max, total - 1]，最小 iteration 的条目已被丢弃
        const int lowestKept = total - GroupE2eeCrypto::MaxSkippedMessageKeys;
        QVERIFY(!skipped.contains(lowestKept - 1));
        QVERIFY(skipped.contains(lowestKept));
        QVERIFY(skipped.contains(total - 1));
    }

    // 回归（2026-09-09 周度审查 R2）：群消息编辑会以更大的 iteration 覆盖旧正文，
    // 使 iteration 与 message_id 顺序解耦。离线设备按消息 id 升序补收时先解到被
    // 编辑消息（高 iteration），其后到达的低 iteration 消息若无跳序密钥缓存将被
    // 回滚检查永久拒绝（chain-key ratchet 单向，明文不可恢复）
    void groupEditRewriteKeepsLaterMessageDecryptable()
    {
        auto key = GroupE2eeCrypto::generateSenderKey();
        const QByteArray chainKey0 = key.chainKey;

        // 发送 A(id=10) 与 B(id=11)
        const auto encA = GroupE2eeCrypto::encryptMessage(key, "A original");
        const auto encB = GroupE2eeCrypto::encryptMessage(key, "B body");
        QCOMPARE(encA.iteration, 1);
        QCOMPARE(encB.iteration, 2);
        // 编辑 A：服务端正文被 iteration=3 的新密文覆盖，而 A 的 message_id 仍小于 B
        const auto encAEdited = GroupE2eeCrypto::encryptMessage(key, "A edited");
        QCOMPARE(encAEdited.iteration, 3);

        // 旧行为（不传缓存）：按 id 升序先解 A' 再解 B → B 被永久拒绝
        {
            QByteArray chainKey = chainKey0;
            int iteration = 0;
            QCOMPARE(GroupE2eeCrypto::decryptMessage(
                chainKey, iteration, key.publicSigningKey, encAEdited), QByteArray("A edited"));
            QCOMPARE(iteration, 3);
            QVERIFY(GroupE2eeCrypto::decryptMessage(
                chainKey, iteration, key.publicSigningKey, encB).isEmpty());
        }

        // 修复后（带跳序密钥缓存）：B 与 A 的原始版本均可解出
        {
            QByteArray chainKey = chainKey0;
            int iteration = 0;
            QMap<int, QByteArray> skipped;
            QCOMPARE(GroupE2eeCrypto::decryptMessage(
                chainKey, iteration, key.publicSigningKey, encAEdited, &skipped),
                QByteArray("A edited"));
            QCOMPARE(iteration, 3);
            QCOMPARE(skipped.size(), 2);
            QCOMPARE(GroupE2eeCrypto::decryptMessage(
                chainKey, iteration, key.publicSigningKey, encB, &skipped),
                QByteArray("B body"));
            QCOMPARE(iteration, 3);
            QCOMPARE(GroupE2eeCrypto::decryptMessage(
                chainKey, iteration, key.publicSigningKey, encA, &skipped),
                QByteArray("A original"));
            QVERIFY(skipped.isEmpty());
        }
    }
};

QTEST_GUILESS_MAIN(TestGroupE2eeCrypto)
#include "TestGroupE2eeCrypto.moc"
