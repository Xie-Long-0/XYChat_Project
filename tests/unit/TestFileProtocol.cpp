/**
 * M8: 文件传输协议与文件加密原语单元测试
 *
 * 覆盖：FileManifest 编解码往返与各类非法输入的拒绝路径、分片数学（分片数取整、
 *       末片余量、参数自洽性、校验和形态）、FileCrypto 的分片 nonce/AAD 派生、
 *       分片加解密与认证失败路径（篡改、错序号、错密钥）、流式摘要与票据。
 */
#include <QtTest>
#include <QJsonDocument>
#include <QJsonObject>

#include "encryption/E2eeCrypto.h"
#include "encryption/FileCrypto.h"
#include "protocol/FileProtocol.h"

using namespace XYChat::Protocol;
using namespace XYChat::Security;

namespace
{
// 构造一份各项参数自洽的清单，供各用例在其上做单点破坏
FileManifest makeValidManifest()
{
    const auto fileKey = FileCrypto::generateFileKey();
    FileManifest m;
    m.fileId = 42;
    m.name = "quarterly-report.pdf";
    m.mime = "application/pdf";
    m.plainSize = 3 * 1024 * 1024;       // 3 MiB 明文，按 1 MiB 分三片
    m.cipherSize = m.plainSize + 3 * 16; // 每片各多一个 GCM 标签
    m.sha256Hex = FileCrypto::sha256Hex("ciphertext-placeholder");
    m.key = fileKey.key;
    m.iv = fileKey.iv;
    return m;
}

// 把清单编码后再取出 JSON 对象，供用例篡改单个字段
QJsonObject manifestObject(const FileManifest &manifest)
{
    const QString encoded = encodeFileManifest(manifest);
    return QJsonDocument::fromJson(encoded.toUtf8()).object();
}

FileManifest decodeObject(const QJsonObject &obj, bool *ok)
{
    return decodeFileManifest(QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)),
                              ok);
}
} // namespace

class TestFileProtocol : public QObject
{
    Q_OBJECT

private slots:
    // 清单编解码
    void manifestRoundTrips();
    void manifestWithMultimediaFieldsRoundTrips();
    void encodeRejectsInvalidManifest();
    void decodeRejectsGarbageAndWrongVersion();
    void decodeRejectsWrongKind();
    void decodeRejectsBadKeyMaterial();
    void decodeRejectsBadSha256();
    void decodeRejectsInconsistentSizes();
    void decodeRejectsBadNameAndOversizeThumbnail();
    void looksLikeFileManifestDiscriminates();

    // 分片数学
    void chunkCountIsCeilingDivision();
    void chunkCountRejectsBadParameters();
    void expectedChunkBytesGivesRemainderForLastChunk();
    void expectedChunkBytesRejectsOutOfRangeIndex();
    void chunkingSelfConsistencyIsEnforced();
    void plainAndCipherChunkSizesDifferByTag();
    void sha256HexFormIsChecked();

    // 文件加密原语
    void fileKeyGenerationIsUniqueAndCorrectlySized();
    void chunkNonceIsUniquePerIndexAndLeavesIvIntact();
    void chunkAadIsBigEndianIndex();
    void chunkEncryptDecryptRoundTrips();
    void emptyChunkStillAuthenticates();
    void decryptRejectsTamperedCiphertext();
    void decryptRejectsWrongChunkIndex();
    void decryptRejectsWrongKey();
    void sha256StreamMatchesOneShot();
    void ticketIsUrlSafeAndHashIsStable();

    // 协议与加密原语的联合口径
    void multiChunkFileMatchesDeclaredChunking();
};

void TestFileProtocol::manifestRoundTrips()
{
    const FileManifest original = makeValidManifest();
    QVERIFY(original.isValid());

    bool ok = false;
    const FileManifest decoded = decodeFileManifest(encodeFileManifest(original), &ok);
    QVERIFY(ok);
    QCOMPARE(decoded.fileId, original.fileId);
    QCOMPARE(decoded.name, original.name);
    QCOMPARE(decoded.mime, original.mime);
    QCOMPARE(decoded.plainSize, original.plainSize);
    QCOMPARE(decoded.cipherSize, original.cipherSize);
    QCOMPARE(decoded.sha256Hex, original.sha256Hex);
    QCOMPARE(decoded.key, original.key);
    QCOMPARE(decoded.iv, original.iv);
    // 未设置的多媒体预留字段应保持默认值
    QCOMPARE(decoded.width, 0);
    QCOMPARE(decoded.height, 0);
    QCOMPARE(decoded.durationMs, qint64(0));
    QVERIFY(decoded.thumbnail.isEmpty());
}

void TestFileProtocol::manifestWithMultimediaFieldsRoundTrips()
{
    FileManifest original = makeValidManifest();
    original.mime = "image/png";
    original.width = 1920;
    original.height = 1080;
    original.durationMs = 0;
    original.thumbnail = QByteArray(1024, 't');
    QVERIFY(original.isValid());

    bool ok = false;
    const FileManifest decoded = decodeFileManifest(encodeFileManifest(original), &ok);
    QVERIFY(ok);
    QCOMPARE(decoded.width, 1920);
    QCOMPARE(decoded.height, 1080);
    QCOMPARE(decoded.thumbnail, original.thumbnail);
}

void TestFileProtocol::encodeRejectsInvalidManifest()
{
    FileManifest broken = makeValidManifest();
    broken.fileId = 0;
    QVERIFY(!broken.isValid());
    // 非法清单不得被编码：调用方据空串拒绝发送，避免把半截元数据发出去
    QVERIFY(encodeFileManifest(broken).isEmpty());

    FileManifest noKey = makeValidManifest();
    noKey.key.clear();
    QVERIFY(encodeFileManifest(noKey).isEmpty());
}

void TestFileProtocol::decodeRejectsGarbageAndWrongVersion()
{
    bool ok = true;
    QVERIFY(decodeFileManifest(QString(), &ok).fileId == 0);
    QVERIFY(!ok);

    ok = true;
    decodeFileManifest("not json at all", &ok);
    QVERIFY(!ok);

    ok = true;
    decodeFileManifest("[1,2,3]", &ok); // 合法 JSON 但不是对象
    QVERIFY(!ok);

    // 版本不符即整体拒绝，不做尽力解析
    QJsonObject obj = manifestObject(makeValidManifest());
    obj["v"] = FileManifestVersion + 1;
    ok = true;
    decodeObject(obj, &ok);
    QVERIFY(!ok);

    obj = manifestObject(makeValidManifest());
    obj.remove("v");
    ok = true;
    decodeObject(obj, &ok);
    QVERIFY(!ok);
}

void TestFileProtocol::decodeRejectsWrongKind()
{
    QJsonObject obj = manifestObject(makeValidManifest());
    obj["kind"] = "text";
    bool ok = true;
    decodeObject(obj, &ok);
    QVERIFY(!ok);

    obj = manifestObject(makeValidManifest());
    obj.remove("kind");
    ok = true;
    decodeObject(obj, &ok);
    QVERIFY(!ok);
}

void TestFileProtocol::decodeRejectsBadKeyMaterial()
{
    // 密钥短于 32 字节
    QJsonObject obj = manifestObject(makeValidManifest());
    obj["key"] = QString::fromLatin1(QByteArray(24, 'k').toBase64());
    bool ok = true;
    decodeObject(obj, &ok);
    QVERIFY(!ok);

    // nonce 前缀短于 12 字节
    obj = manifestObject(makeValidManifest());
    obj["iv"] = QString::fromLatin1(QByteArray(8, 'i').toBase64());
    ok = true;
    decodeObject(obj, &ok);
    QVERIFY(!ok);

    // 缺失密钥字段
    obj = manifestObject(makeValidManifest());
    obj.remove("key");
    ok = true;
    decodeObject(obj, &ok);
    QVERIFY(!ok);

    // 非法 base64（严格解码返回空 -> 长度校验失败）
    obj = manifestObject(makeValidManifest());
    obj["key"] = "!!!not-base64!!!";
    ok = true;
    decodeObject(obj, &ok);
    QVERIFY(!ok);
}

void TestFileProtocol::decodeRejectsBadSha256()
{
    const FileManifest valid = makeValidManifest();

    // 长度不足
    QJsonObject obj = manifestObject(valid);
    obj["sha256"] = "abcd1234";
    bool ok = true;
    decodeObject(obj, &ok);
    QVERIFY(!ok);

    // 大写十六进制：与服务端 toHex() 的小写输出口径不一致，一律拒绝，
    // 否则同一份密文会有两种"合法"校验和写法
    obj = manifestObject(valid);
    obj["sha256"] = valid.sha256Hex.toUpper();
    ok = true;
    decodeObject(obj, &ok);
    QVERIFY(!ok);

    // 含非十六进制字符
    obj = manifestObject(valid);
    obj["sha256"] = valid.sha256Hex.left(63) + "z";
    ok = true;
    decodeObject(obj, &ok);
    QVERIFY(!ok);
}

void TestFileProtocol::decodeRejectsInconsistentSizes()
{
    // 密文与明文等长：每个分片都会追加 GCM 标签，等长意味着加密环节被跳过
    QJsonObject obj = manifestObject(makeValidManifest());
    obj["cipherSize"] = obj.value("plainSize").toVariant().toLongLong();
    bool ok = true;
    decodeObject(obj, &ok);
    QVERIFY(!ok);

    // 密文短于明文
    obj = manifestObject(makeValidManifest());
    obj["cipherSize"] = 16;
    ok = true;
    decodeObject(obj, &ok);
    QVERIFY(!ok);

    // 超出单文件体积上限
    obj = manifestObject(makeValidManifest());
    obj["cipherSize"] = static_cast<double>(MaxFileSize) + 1.0;
    ok = true;
    decodeObject(obj, &ok);
    QVERIFY(!ok);

    // 负明文大小
    obj = manifestObject(makeValidManifest());
    obj["plainSize"] = -1;
    ok = true;
    decodeObject(obj, &ok);
    QVERIFY(!ok);

    // fileId 缺失或非正
    obj = manifestObject(makeValidManifest());
    obj["fileId"] = 0;
    ok = true;
    decodeObject(obj, &ok);
    QVERIFY(!ok);
}

void TestFileProtocol::decodeRejectsBadNameAndOversizeThumbnail()
{
    // 文件名必填：空名会让接收端落地路径退化为用户目录
    QJsonObject obj = manifestObject(makeValidManifest());
    obj["name"] = "";
    bool ok = true;
    decodeObject(obj, &ok);
    QVERIFY(!ok);

    obj = manifestObject(makeValidManifest());
    obj.remove("name");
    ok = true;
    decodeObject(obj, &ok);
    QVERIFY(!ok);

    // 文件名超长
    obj = manifestObject(makeValidManifest());
    obj["name"] = QString(MaxFileNameLength + 1, 'n');
    ok = true;
    decodeObject(obj, &ok);
    QVERIFY(!ok);

    // 缩略图超上限：内联缩略图会随消息正文进入 envelope，
    // 过大将挤占 MaxPayloadSize 并拖慢每条消息的解密
    obj = manifestObject(makeValidManifest());
    obj["thumb"] = QString::fromLatin1(QByteArray(MaxThumbnailBytes + 1, 'x').toBase64());
    ok = true;
    decodeObject(obj, &ok);
    QVERIFY(!ok);

    // 负的尺寸/时长
    obj = manifestObject(makeValidManifest());
    obj["width"] = -1;
    ok = true;
    decodeObject(obj, &ok);
    QVERIFY(!ok);
}

void TestFileProtocol::looksLikeFileManifestDiscriminates()
{
    const FileManifest valid = makeValidManifest();
    QVERIFY(looksLikeFileManifest(encodeFileManifest(valid)));

    // 普通文本与 E2EE envelope 都不应被误判为清单
    QVERIFY(!looksLikeFileManifest("hello world"));
    QVERIFY(!looksLikeFileManifest(QString()));
    QVERIFY(!looksLikeFileManifest("{\"v\":1,\"devices\":[]}"));
    QVERIFY(!looksLikeFileManifest("plain text that mentions \"kind\":\"file\" only"));
}

void TestFileProtocol::chunkCountIsCeilingDivision()
{
    QCOMPARE(chunkCountFor(MinChunkSize, MinChunkSize), 1);
    QCOMPARE(chunkCountFor(MinChunkSize + 1, MinChunkSize), 2);
    QCOMPARE(chunkCountFor(3 * DefaultChunkSize, DefaultChunkSize), 3);
    QCOMPARE(chunkCountFor(3 * DefaultChunkSize + 1, DefaultChunkSize), 4);
    // 空文件的密文只有一个标签长度，仍算一片
    QCOMPARE(chunkCountFor(16, MinChunkSize), 1);
}

void TestFileProtocol::chunkCountRejectsBadParameters()
{
    QCOMPARE(chunkCountFor(0, DefaultChunkSize), 0);
    QCOMPARE(chunkCountFor(-1, DefaultChunkSize), 0);
    QCOMPARE(chunkCountFor(1024, 0), 0);
    // 分片大小低于下界（过小分片会让 100MB 文件产生海量请求）
    QCOMPARE(chunkCountFor(1024 * 1024, 1024), 0);
    // 分片大小超上界
    QCOMPARE(chunkCountFor(1024 * 1024, MaxChunkSize + 1), 0);
    // 体积超上限
    QCOMPARE(chunkCountFor(MaxFileSize + 1, DefaultChunkSize), 0);
}

void TestFileProtocol::expectedChunkBytesGivesRemainderForLastChunk()
{
    const qint64 chunkSize = DefaultChunkSize;
    const qint64 cipherSize = 2 * chunkSize + 12345;
    const int chunkCount = chunkCountFor(cipherSize, chunkSize);
    QCOMPARE(chunkCount, 3);
    QCOMPARE(expectedChunkBytes(cipherSize, chunkSize, chunkCount, 0), chunkSize);
    QCOMPARE(expectedChunkBytes(cipherSize, chunkSize, chunkCount, 1), chunkSize);
    QCOMPARE(expectedChunkBytes(cipherSize, chunkSize, chunkCount, 2), qint64(12345));
    // 三片之和恰为总体积
    qint64 total = 0;
    for (int i = 0; i < chunkCount; ++i) {
        total += expectedChunkBytes(cipherSize, chunkSize, chunkCount, i);
    }
    QCOMPARE(total, cipherSize);
}

void TestFileProtocol::expectedChunkBytesRejectsOutOfRangeIndex()
{
    const qint64 cipherSize = 2 * DefaultChunkSize;
    const int chunkCount = chunkCountFor(cipherSize, DefaultChunkSize);
    QCOMPARE(expectedChunkBytes(cipherSize, DefaultChunkSize, chunkCount, -1), qint64(-1));
    QCOMPARE(expectedChunkBytes(cipherSize, DefaultChunkSize, chunkCount, chunkCount), qint64(-1));
    // 分片参数本身不自洽时同样拒绝
    QCOMPARE(expectedChunkBytes(cipherSize, DefaultChunkSize, chunkCount + 1, 0), qint64(-1));
}

void TestFileProtocol::chunkingSelfConsistencyIsEnforced()
{
    const qint64 cipherSize = 3 * DefaultChunkSize;
    QVERIFY(isChunkingValid(cipherSize, DefaultChunkSize, 3));

    // 少报分片数：否则可用少量超大分片绕过 MaxChunkCount 与体积上限的联合约束
    QVERIFY(!isChunkingValid(cipherSize, DefaultChunkSize, 2));
    // 多报分片数
    QVERIFY(!isChunkingValid(cipherSize, DefaultChunkSize, 4));
    QVERIFY(!isChunkingValid(cipherSize, DefaultChunkSize, 0));
    QVERIFY(!isChunkingValid(0, DefaultChunkSize, 0));
    QVERIFY(!isChunkingValid(cipherSize, MinChunkSize - 1, 1));
    QVERIFY(!isChunkingValid(cipherSize, MaxChunkSize + 1, 1));
    QVERIFY(!isChunkingValid(MaxFileSize + 1, DefaultChunkSize, 1));

    // 分片数上限：2 GiB / 64 KiB = 32768 片，远超 MaxChunkCount，必须被拦下
    const qint64 many = MaxFileSize;
    QVERIFY(!isChunkingValid(many, MinChunkSize, chunkCountFor(many, MinChunkSize)));
    QVERIFY(chunkCountFor(many, MinChunkSize) > MaxChunkCount);
}

void TestFileProtocol::plainAndCipherChunkSizesDifferByTag()
{
    QCOMPARE(cipherSizeOfChunk(1024), qint64(1024 + 16));
    QCOMPARE(cipherSizeOfChunk(0), qint64(16)); // 空明文分片仍带一个标签
    QCOMPARE(cipherSizeOfChunk(-1), qint64(-1));
    QCOMPARE(plainSizeOfChunk(16), qint64(0));
    QCOMPARE(plainSizeOfChunk(1024 + 16), qint64(1024));
    // 不足一个标签的"密文分片"不可能是合法输出
    QCOMPARE(plainSizeOfChunk(15), qint64(-1));
    QCOMPARE(plainSizeOfChunk(0), qint64(-1));
}

void TestFileProtocol::sha256HexFormIsChecked()
{
    QVERIFY(isSha256Hex(FileCrypto::sha256Hex("anything")));
    QVERIFY(!isSha256Hex(""));
    QVERIFY(!isSha256Hex(QString(63, 'a')));
    QVERIFY(!isSha256Hex(QString(65, 'a')));
    QVERIFY(!isSha256Hex(FileCrypto::sha256Hex("anything").toUpper()));
    QVERIFY(!isSha256Hex(QString(63, 'a') + "g"));
}

void TestFileProtocol::fileKeyGenerationIsUniqueAndCorrectlySized()
{
    const auto a = FileCrypto::generateFileKey();
    const auto b = FileCrypto::generateFileKey();
    QVERIFY(a.valid && b.valid);
    QCOMPARE(a.key.size(), 32);
    QCOMPARE(a.iv.size(), 12);
    // 跨文件复用同一密钥会让 nonce 空间被两个文件共享，必须每次独立生成
    QVERIFY(a.key != b.key);
    QVERIFY(a.iv != b.iv);
}

void TestFileProtocol::chunkNonceIsUniquePerIndexAndLeavesIvIntact()
{
    const auto fileKey = FileCrypto::generateFileKey();
    const QByteArray ivBefore = fileKey.iv;

    const QByteArray n0 = FileCrypto::chunkNonce(fileKey.iv, 0);
    const QByteArray n1 = FileCrypto::chunkNonce(fileKey.iv, 1);
    const QByteArray n2 = FileCrypto::chunkNonce(fileKey.iv, 2);
    QCOMPARE(n0.size(), 12);
    QVERIFY(n0 != n1);
    QVERIFY(n1 != n2);
    QVERIFY(n0 != n2);
    // 确定性：同一 (iv, index) 必须派生出同一 nonce，否则接收端无法重建
    QCOMPARE(FileCrypto::chunkNonce(fileKey.iv, 7), FileCrypto::chunkNonce(fileKey.iv, 7));
    // 派生不得改动调用方的 iv（QByteArray 隐式共享，写入前必须 detach）
    QCOMPARE(fileKey.iv, ivBefore);

    // 前 8 字节保持 iv 原样，只有尾部 4 字节参与序号异或
    QCOMPARE(n0.left(8), ivBefore.left(8));

    // 非法入参
    QVERIFY(FileCrypto::chunkNonce(QByteArray(11, 'x'), 0).isEmpty());
    QVERIFY(FileCrypto::chunkNonce(fileKey.iv, -1).isEmpty());
}

void TestFileProtocol::chunkAadIsBigEndianIndex()
{
    const QByteArray aad1 = FileCrypto::chunkAad(1);
    QCOMPARE(aad1.size(), 4);
    QCOMPARE(aad1, QByteArray("\x00\x00\x00\x01", 4));
    QCOMPARE(FileCrypto::chunkAad(0x01020304), QByteArray("\x01\x02\x03\x04", 4));
    QVERIFY(FileCrypto::chunkAad(-1).isEmpty());
    // AAD 随序号变化，这是"以他片冒替"会被 GCM 认证拒掉的原因
    QVERIFY(FileCrypto::chunkAad(1) != FileCrypto::chunkAad(2));
}

void TestFileProtocol::chunkEncryptDecryptRoundTrips()
{
    const auto fileKey = FileCrypto::generateFileKey();
    const QByteArray plaintext("the quick brown fox jumps over the lazy dog");

    const QByteArray cipher = FileCrypto::encryptChunk(fileKey.key, fileKey.iv, 0, plaintext);
    QVERIFY(!cipher.isEmpty());
    QCOMPARE(cipher.size(), plaintext.size() + 16);
    // 密文不得包含明文片段
    QVERIFY(!cipher.contains(plaintext.left(8)));

    QCOMPARE(FileCrypto::decryptChunk(fileKey.key, fileKey.iv, 0, cipher), plaintext);

    // 各分片密文互不相同（nonce 随序号变化）
    const QByteArray cipher1 = FileCrypto::encryptChunk(fileKey.key, fileKey.iv, 1, plaintext);
    QVERIFY(cipher1 != cipher);
    QCOMPARE(FileCrypto::decryptChunk(fileKey.key, fileKey.iv, 1, cipher1), plaintext);
}

void TestFileProtocol::emptyChunkStillAuthenticates()
{
    const auto fileKey = FileCrypto::generateFileKey();
    const QByteArray cipher = FileCrypto::encryptChunk(fileKey.key, fileKey.iv, 0, QByteArray());
    // 空明文分片仍产出 16 字节标签，因此"成功"与"失败"可用空返回值区分
    QCOMPARE(cipher.size(), 16);
    QCOMPARE(FileCrypto::decryptChunk(fileKey.key, fileKey.iv, 0, cipher), QByteArray());
    // 篡改标签后必须认证失败
    QByteArray tampered = cipher;
    tampered[0] = static_cast<char>(tampered.at(0) ^ 0x01);
    QVERIFY(FileCrypto::decryptChunk(fileKey.key, fileKey.iv, 0, tampered).isEmpty());
}

void TestFileProtocol::decryptRejectsTamperedCiphertext()
{
    const auto fileKey = FileCrypto::generateFileKey();
    const QByteArray plaintext(4096, 'p');
    QByteArray cipher = FileCrypto::encryptChunk(fileKey.key, fileKey.iv, 3, plaintext);
    QVERIFY(!cipher.isEmpty());

    // 翻转密文中部一位
    QByteArray tampered = cipher;
    tampered[100] = static_cast<char>(tampered.at(100) ^ 0x80);
    QVERIFY(FileCrypto::decryptChunk(fileKey.key, fileKey.iv, 3, tampered).isEmpty());

    // 截断（少一个字节）
    QVERIFY(FileCrypto::decryptChunk(fileKey.key, fileKey.iv, 3, cipher.left(cipher.size() - 1)).isEmpty());
    // 短于一个标签
    QVERIFY(FileCrypto::decryptChunk(fileKey.key, fileKey.iv, 3, QByteArray(8, 'x')).isEmpty());
}

void TestFileProtocol::decryptRejectsWrongChunkIndex()
{
    const auto fileKey = FileCrypto::generateFileKey();
    const QByteArray plaintext(2048, 'q');
    const QByteArray cipher = FileCrypto::encryptChunk(fileKey.key, fileKey.iv, 5, plaintext);
    QVERIFY(!cipher.isEmpty());

    // AAD 绑定分片位置：用相邻序号解密必须失败，否则攻击者可重排分片顺序
    QVERIFY(FileCrypto::decryptChunk(fileKey.key, fileKey.iv, 4, cipher).isEmpty());
    QVERIFY(FileCrypto::decryptChunk(fileKey.key, fileKey.iv, 6, cipher).isEmpty());
    QVERIFY(FileCrypto::decryptChunk(fileKey.key, fileKey.iv, 0, cipher).isEmpty());
    // 正确序号仍可解
    QCOMPARE(FileCrypto::decryptChunk(fileKey.key, fileKey.iv, 5, cipher), plaintext);
}

void TestFileProtocol::decryptRejectsWrongKey()
{
    const auto a = FileCrypto::generateFileKey();
    const auto b = FileCrypto::generateFileKey();
    const QByteArray plaintext(1024, 'r');
    const QByteArray cipher = FileCrypto::encryptChunk(a.key, a.iv, 0, plaintext);

    // 换密钥
    QVERIFY(FileCrypto::decryptChunk(b.key, a.iv, 0, cipher).isEmpty());
    // 同密钥换 iv（即换 nonce 前缀）
    QVERIFY(FileCrypto::decryptChunk(a.key, b.iv, 0, cipher).isEmpty());
    // 密钥长度非法
    QVERIFY(FileCrypto::encryptChunk(QByteArray(16, 'k'), a.iv, 0, plaintext).isEmpty());
    QVERIFY(FileCrypto::decryptChunk(QByteArray(16, 'k'), a.iv, 0, cipher).isEmpty());
}

void TestFileProtocol::sha256StreamMatchesOneShot()
{
    const QByteArray part1(1000, 'a');
    const QByteArray part2(2000, 'b');
    const QByteArray part3(1, 'c');

    FileCrypto::Sha256Stream stream;
    stream.addData(part1);
    stream.addData(part2);
    stream.addData(part3);
    const QString streamed = stream.hexDigest();

    QCOMPARE(streamed, FileCrypto::sha256Hex(part1 + part2 + part3));
    QVERIFY(isSha256Hex(streamed));
    // 内容不同则摘要不同
    FileCrypto::Sha256Stream other;
    other.addData(part1);
    other.addData(part2);
    QVERIFY(other.hexDigest() != streamed);
    // 空输入不影响摘要（跳过 addData 与传入空数组等价）
    FileCrypto::Sha256Stream withEmpty;
    withEmpty.addData(part1);
    withEmpty.addData(QByteArray());
    withEmpty.addData(part2);
    withEmpty.addData(part3);
    QCOMPARE(withEmpty.hexDigest(), streamed);
}

void TestFileProtocol::ticketIsUrlSafeAndHashIsStable()
{
    const QString ticket = FileCrypto::generateTicket();
    QVERIFY(!ticket.isEmpty());
    // base64url 且无填充：票据要经 URL 路径段传输，不能含 + / =
    QVERIFY(!ticket.contains('+'));
    QVERIFY(!ticket.contains('/'));
    QVERIFY(!ticket.contains('='));
    QVERIFY(ticket.size() >= 40); // 32 字节随机数的 base64 长度

    // 每次签发的票据都不同（重复使用同一票据会让一次泄露影响全部上传）
    QVERIFY(FileCrypto::generateTicket() != ticket);

    const QString hash = FileCrypto::ticketHash(ticket);
    QVERIFY(isSha256Hex(hash));
    // 摘要稳定且不可逆推：不同票据摘要不同
    QCOMPARE(FileCrypto::ticketHash(ticket), hash);
    QVERIFY(FileCrypto::ticketHash(FileCrypto::generateTicket()) != hash);
    // 摘要不等于票据本身（服务端只存摘要）
    QVERIFY(hash != ticket);
    QVERIFY(FileCrypto::ticketHash(QString()).isEmpty());
}

void TestFileProtocol::multiChunkFileMatchesDeclaredChunking()
{
    // 用最小分片口径构造一个跨三片的文件，验证客户端与服务端算出的分片数、
    // 每片字节数完全一致：任一侧口径漂移都会在 finalize 时被判为长度不符
    const qint64 chunkSize = MinChunkSize;                 // 密文分片 64 KiB
    const qint64 plainChunk = plainSizeOfChunk(chunkSize); // 对应明文分片
    QVERIFY(plainChunk > 0);

    const qint64 lastPlain = 100;
    const QByteArray plaintext(plainChunk * 2 + lastPlain, 'x');
    const qint64 cipherSize = cipherSizeOfChunk(plainChunk) * 2 + cipherSizeOfChunk(lastPlain);
    const int chunkCount = chunkCountFor(cipherSize, chunkSize);
    QVERIFY(isChunkingValid(cipherSize, chunkSize, chunkCount));
    QCOMPARE(chunkCount, 3);
    QCOMPARE(expectedChunkBytes(cipherSize, chunkSize, chunkCount, 2),
             cipherSizeOfChunk(lastPlain));

    // 按分片加密后逐片核对长度，并验证整体摘要可被流式重建
    const auto fileKey = FileCrypto::generateFileKey();
    FileCrypto::Sha256Stream digest;
    QByteArray assembled;
    qint64 plainOffset = 0;
    for (int i = 0; i < chunkCount; ++i) {
        const qint64 expectedCipher = expectedChunkBytes(cipherSize, chunkSize, chunkCount, i);
        const qint64 expectedPlain = plainSizeOfChunk(expectedCipher);
        QVERIFY(expectedPlain >= 0);
        const QByteArray piece = plaintext.mid(plainOffset, expectedPlain);
        QCOMPARE(static_cast<qint64>(piece.size()), expectedPlain);

        const QByteArray encrypted = FileCrypto::encryptChunk(fileKey.key, fileKey.iv, i, piece);
        QCOMPARE(static_cast<qint64>(encrypted.size()), expectedCipher);
        QCOMPARE(FileCrypto::decryptChunk(fileKey.key, fileKey.iv, i, encrypted), piece);

        digest.addData(encrypted);
        assembled.append(encrypted);
        plainOffset += expectedPlain;
    }
    QCOMPARE(plainOffset, static_cast<qint64>(plaintext.size()));
    QCOMPARE(static_cast<qint64>(assembled.size()), cipherSize);
    // 流式摘要与整体摘要一致：服务端组装时正是这样核对客户端声明的校验和
    QCOMPARE(digest.hexDigest(), FileCrypto::sha256Hex(assembled));
}

QTEST_GUILESS_MAIN(TestFileProtocol)
#include "TestFileProtocol.moc"
