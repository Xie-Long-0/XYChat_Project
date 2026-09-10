/**
 * M8: 本地对象存储实现单元测试
 *
 * 覆盖：目录骨架与存储键形态、分片写入/列举/读取、finalize 的成功与各失败分类
 *       （分片缺失、长度不符、校验和不符、参数非法）、幂等重入、Range 读取、
 *       删除与幂等，以及对非法存储键（含路径穿越尝试）的 fail-closed 拒绝。
 *
 * 断点续传的存储层不变量在此锁定：finalize 因分片缺失而失败时必须保留已收分片，
 * 补传缺片后可直接完成，不得要求整文件重传。
 */
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QList>
#include <QTemporaryDir>

#include "encryption/FileCrypto.h"
#include "protocol/FileProtocol.h"
#include "storage/LocalFileStorage.h"

using namespace XYChat::Server;
using namespace XYChat::Security;
using namespace XYChat::Protocol;

namespace
{
struct ChunkedPayload
{
    QList<QByteArray> chunks;
    qint64 totalSize = 0;
    QString sha256Hex;

    QByteArray assembled() const
    {
        QByteArray all;
        for (const QByteArray &chunk : chunks) {
            all.append(chunk);
        }
        return all;
    }
};

// 构造 chunkCount 片的密文负载：前 count-1 片为 MinChunkSize，末片为 lastChunkBytes，
// 与 Protocol::expectedChunkBytes 的口径一致
ChunkedPayload makePayload(int chunkCount, qint64 lastChunkBytes, char filler)
{
    ChunkedPayload payload;
    FileCrypto::Sha256Stream digest;
    for (int i = 0; i < chunkCount; ++i) {
        const qint64 size = (i == chunkCount - 1) ? lastChunkBytes : MinChunkSize;
        QByteArray chunk(size, filler);
        // 让每片内容互不相同，否则分片被重排也测不出来
        chunk[0] = static_cast<char>(chunk.at(0) ^ static_cast<char>(i + 1));
        payload.chunks.append(chunk);
        digest.addData(chunk);
        payload.totalSize += size;
    }
    payload.sha256Hex = digest.hexDigest();
    return payload;
}
} // namespace

class TestObjectStorage : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void initializeCreatesDirectorySkeleton();
    void allocateBlobKeyIsRandomAndWellFormed();
    void rejectsInvalidAndTraversingBlobKeys();
    void putChunkListsAndReadsBack();
    void receivedChunksAreSortedRegardlessOfInsertOrder();
    void putChunkOverwritesIdempotently();
    void putChunkRejectsEmptyDataAndBadIndex();
    void receivedChunksIgnoresForeignFiles();
    void finalizeAssemblesAndVerifiesChecksum();
    void finalizeReportsIncompleteAndKeepsChunksForResume();
    void finalizeReportsChunkSizeMismatch();
    void finalizeReportsChecksumMismatch();
    void finalizeIsIdempotentAfterSuccess();
    void finalizeRejectsInvalidChunking();
    void readRangeServesPartialContent();
    void removeDeletesPartsAndBlobIdempotently();
    void removeCleansStaleTmpFilesFromCrashedRun();

private:
    QTemporaryDir m_tempDir;
    LocalFileStorage *m_storage = nullptr;
    QString m_root;
    int m_caseCounter = 0;
};

void TestObjectStorage::initTestCase()
{
    QVERIFY(m_tempDir.isValid());
}

void TestObjectStorage::init()
{
    // 每个用例独立子目录，避免上一个用例的残留影响断言
    m_root = m_tempDir.path() + "/case-" + QString::number(++m_caseCounter);
    m_storage = new LocalFileStorage(m_root);
    QVERIFY(m_storage->initialize());
}

void TestObjectStorage::cleanup()
{
    delete m_storage;
    m_storage = nullptr;
    QDir(m_root).removeRecursively();
}

void TestObjectStorage::initializeCreatesDirectorySkeleton()
{
    QVERIFY(QDir(m_root + "/parts").exists());
    QVERIFY(QDir(m_root + "/tmp").exists());
    QVERIFY(QDir(m_root + "/blobs").exists());
    QCOMPARE(m_storage->rootPath(), m_root);
    // 重复初始化不应失败（服务端重启走同一路径）
    QVERIFY(m_storage->initialize());
}

void TestObjectStorage::allocateBlobKeyIsRandomAndWellFormed()
{
    const QString key = m_storage->allocateBlobKey();
    QCOMPARE(key.size(), LocalFileStorage::BlobKeyLength);
    QVERIFY(m_storage->isValidBlobKey(key));
    // 全小写十六进制：与服务端各处的 toHex() 输出口径一致
    QCOMPARE(key, key.toLower());
    // 连续分配必须互不相同（可预测的键会让存储路径变成可枚举的对象列表）
    QVERIFY(m_storage->allocateBlobKey() != key);
    QVERIFY(m_storage->allocateBlobKey() != m_storage->allocateBlobKey());
}

void TestObjectStorage::rejectsInvalidAndTraversingBlobKeys()
{
    QVERIFY(!m_storage->isValidBlobKey(QString()));
    QVERIFY(!m_storage->isValidBlobKey("abc"));
    QVERIFY(!m_storage->isValidBlobKey(QString(32, 'Z')));  // 非十六进制
    QVERIFY(!m_storage->isValidBlobKey(QString(32, 'A')));  // 大写
    QVERIFY(!m_storage->isValidBlobKey(QString(31, 'a')));  // 长度不足
    QVERIFY(!m_storage->isValidBlobKey(QString(33, 'a')));  // 长度超出
    QVERIFY(!m_storage->isValidBlobKey(QString(30, 'a') + "/."));
    QVERIFY(!m_storage->isValidBlobKey(QString(30, 'a') + ".."));

    // 路径穿越尝试在每个入口都被挡下，且不得在磁盘上留下任何痕迹
    const QString evil = "../../../../etc/passwd";
    const QString sha = FileCrypto::sha256Hex("x");
    QVERIFY(!m_storage->putChunk(evil, 0, QByteArray("data")));
    QVERIFY(m_storage->receivedChunks(evil).isEmpty());
    QVERIFY(m_storage->readChunk(evil, 0).isEmpty());
    QVERIFY(!m_storage->isFinalized(evil));
    QCOMPARE(m_storage->blobSize(evil), qint64(-1));
    QVERIFY(m_storage->readRange(evil, 0, 10).isEmpty());
    // 非法键不返回"已删除"：掩盖调用方的键来源错误比幂等更危险
    QVERIFY(!m_storage->remove(evil));
    QCOMPARE(m_storage->finalize(evil, MinChunkSize, MinChunkSize, 1, sha),
             IObjectStorage::FinalizeStatus::InvalidArguments);
    QVERIFY(!QFile::exists(m_root + "/../../etc/passwd"));
}

void TestObjectStorage::putChunkListsAndReadsBack()
{
    const QString key = m_storage->allocateBlobKey();
    QVERIFY(m_storage->receivedChunks(key).isEmpty());

    const QByteArray c0(1000, 'a');
    const QByteArray c1(500, 'b');
    QVERIFY(m_storage->putChunk(key, 0, c0));
    QVERIFY(m_storage->putChunk(key, 1, c1));
    QCOMPARE(m_storage->readChunk(key, 0), c0);
    QCOMPARE(m_storage->readChunk(key, 1), c1);
    // 未上传的分片读作空（密文分片至少含 16 字节标签，空即"没有"）
    QVERIFY(m_storage->readChunk(key, 2).isEmpty());

    const QList<int> received = m_storage->receivedChunks(key);
    QCOMPARE(received.size(), 2);
    QCOMPARE(received.at(0), 0);
    QCOMPARE(received.at(1), 1);

    // 分片落在按 blobKey 前两位分桶的目录下
    QVERIFY(QDir(m_root + "/parts/" + key.left(2) + "/" + key).exists());
    // 未组装前不应存在最终对象
    QVERIFY(!m_storage->isFinalized(key));
    QCOMPARE(m_storage->blobSize(key), qint64(-1));
}

void TestObjectStorage::receivedChunksAreSortedRegardlessOfInsertOrder()
{
    const QString key = m_storage->allocateBlobKey();
    // 断点续传时客户端按任意顺序补传，列举结果必须升序，
    // 否则 finalize 的"索引连续覆盖 0..count-1"判定会误报缺片
    QVERIFY(m_storage->putChunk(key, 2, QByteArray(300, 'c')));
    QVERIFY(m_storage->putChunk(key, 0, QByteArray(100, 'a')));
    QVERIFY(m_storage->putChunk(key, 1, QByteArray(200, 'b')));

    QList<int> expected;
    expected << 0 << 1 << 2;
    QCOMPARE(m_storage->receivedChunks(key), expected);
}

void TestObjectStorage::putChunkOverwritesIdempotently()
{
    const QString key = m_storage->allocateBlobKey();
    QVERIFY(m_storage->putChunk(key, 0, QByteArray(100, 'a')));
    // 客户端重试同一分片：覆盖而非追加，也不产生重复条目
    QVERIFY(m_storage->putChunk(key, 0, QByteArray(100, 'b')));
    QCOMPARE(m_storage->readChunk(key, 0), QByteArray(100, 'b'));
    QCOMPARE(m_storage->receivedChunks(key).size(), 1);
}

void TestObjectStorage::putChunkRejectsEmptyDataAndBadIndex()
{
    const QString key = m_storage->allocateBlobKey();
    // 空分片拒绝：密文分片至少含 16 字节 GCM 标签，收下空数据只会让
    // receivedChunks 报告一个 finalize 阶段必然长度不符的分片
    QVERIFY(!m_storage->putChunk(key, 0, QByteArray()));
    QVERIFY(!m_storage->putChunk(key, -1, QByteArray(16, 'a')));
    QVERIFY(!m_storage->putChunk(key, MaxChunkCount, QByteArray(16, 'a')));
    QVERIFY(!m_storage->putChunk(key, MaxChunkCount + 1000, QByteArray(16, 'a')));
    // 全部被拒后磁盘上不留痕迹
    QVERIFY(m_storage->receivedChunks(key).isEmpty());
    QVERIFY(!QDir(m_root + "/parts/" + key.left(2) + "/" + key).exists());
}

void TestObjectStorage::receivedChunksIgnoresForeignFiles()
{
    const QString key = m_storage->allocateBlobKey();
    QVERIFY(m_storage->putChunk(key, 0, QByteArray(16, 'a')));

    const QDir partsDir(m_root + "/parts/" + key.left(2) + "/" + key);
    QVERIFY(partsDir.exists());
    const QStringList foreign = {"notes.txt", "abc.part", "-1.part", "99999.part", ".part"};
    for (const QString &name : foreign) {
        QFile file(partsDir.path() + "/" + name);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("junk"), qint64(4));
        file.close();
    }

    // 只有形态正确的分片名被计入（越界序号、负序号、非数字、空名一律忽略）
    const QList<int> received = m_storage->receivedChunks(key);
    QCOMPARE(received.size(), 1);
    QCOMPARE(received.at(0), 0);
}

void TestObjectStorage::finalizeAssemblesAndVerifiesChecksum()
{
    const ChunkedPayload payload = makePayload(3, 1234, 'z');
    QVERIFY(isChunkingValid(payload.totalSize, MinChunkSize, payload.chunks.size()));

    const QString key = m_storage->allocateBlobKey();
    for (int i = 0; i < payload.chunks.size(); ++i) {
        QVERIFY(m_storage->putChunk(key, i, payload.chunks.at(i)));
    }

    QCOMPARE(m_storage->finalize(key, payload.totalSize, MinChunkSize,
                                 payload.chunks.size(), payload.sha256Hex),
             IObjectStorage::FinalizeStatus::Ok);
    QVERIFY(m_storage->isFinalized(key));
    QCOMPARE(m_storage->blobSize(key), payload.totalSize);
    // 组装成功后回收分片：否则同一份数据在盘上占两倍空间
    QVERIFY(m_storage->receivedChunks(key).isEmpty());
    // 中间产物不得残留（临时文件名带随机后缀，此处按前缀枚举检查）
    QVERIFY(QDir(m_root + "/tmp/" + key.left(2))
                .entryList(QStringList{key + ".*"}, QDir::Files)
                .isEmpty());
    QCOMPARE(m_storage->readRange(key, 0, payload.totalSize), payload.assembled());
}

void TestObjectStorage::finalizeReportsIncompleteAndKeepsChunksForResume()
{
    const ChunkedPayload payload = makePayload(3, 1234, 'z');
    const QString key = m_storage->allocateBlobKey();
    // 只上传第 0 与第 2 片，模拟断网中断
    QVERIFY(m_storage->putChunk(key, 0, payload.chunks.at(0)));
    QVERIFY(m_storage->putChunk(key, 2, payload.chunks.at(2)));

    QCOMPARE(m_storage->finalize(key, payload.totalSize, MinChunkSize,
                                 payload.chunks.size(), payload.sha256Hex),
             IObjectStorage::FinalizeStatus::Incomplete);
    QVERIFY(!m_storage->isFinalized(key));
    // 关键不变量：失败后保留已收分片，客户端只需补传缺的那一片
    QCOMPARE(m_storage->receivedChunks(key).size(), 2);

    QVERIFY(m_storage->putChunk(key, 1, payload.chunks.at(1)));
    QCOMPARE(m_storage->finalize(key, payload.totalSize, MinChunkSize,
                                 payload.chunks.size(), payload.sha256Hex),
             IObjectStorage::FinalizeStatus::Ok);
    QVERIFY(m_storage->isFinalized(key));

    // 数量对得上但索引不连续（缺 0 号、多一个越界号）同样判为缺失
    const QString key2 = m_storage->allocateBlobKey();
    QVERIFY(m_storage->putChunk(key2, 1, payload.chunks.at(0)));
    QVERIFY(m_storage->putChunk(key2, 2, payload.chunks.at(1)));
    QCOMPARE(m_storage->finalize(key2, 2 * MinChunkSize, MinChunkSize, 2,
                                 FileCrypto::sha256Hex(payload.chunks.at(0)
                                                       + payload.chunks.at(1))),
             IObjectStorage::FinalizeStatus::Incomplete);
    QVERIFY(!m_storage->isFinalized(key2));
}

void TestObjectStorage::finalizeReportsChunkSizeMismatch()
{
    const ChunkedPayload payload = makePayload(3, 1234, 'z');
    const QString key = m_storage->allocateBlobKey();
    for (int i = 0; i < payload.chunks.size(); ++i) {
        QVERIFY(m_storage->putChunk(key, i, payload.chunks.at(i)));
    }
    // 把中间一片换成错误长度：客户端不得自行选择分片边界，
    // 否则可用少量超大分片绕过 MaxChunkCount 与 MaxFileSize 的联合约束
    QVERIFY(m_storage->putChunk(key, 1, QByteArray(999, 'b')));

    QCOMPARE(m_storage->finalize(key, payload.totalSize, MinChunkSize,
                                 payload.chunks.size(), payload.sha256Hex),
             IObjectStorage::FinalizeStatus::ChunkSizeMismatch);
    QVERIFY(!m_storage->isFinalized(key));
    QVERIFY(QDir(m_root + "/tmp/" + key.left(2))
                .entryList(QStringList{key + ".*"}, QDir::Files)
                .isEmpty());
}

void TestObjectStorage::finalizeReportsChecksumMismatch()
{
    const ChunkedPayload payload = makePayload(2, MinChunkSize, 'y');
    const QString key = m_storage->allocateBlobKey();
    for (int i = 0; i < payload.chunks.size(); ++i) {
        QVERIFY(m_storage->putChunk(key, i, payload.chunks.at(i)));
    }

    // 分片齐备且长度正确，但客户端声明的整体摘要不符（存储损坏或声明有误）
    QCOMPARE(m_storage->finalize(key, payload.totalSize, MinChunkSize,
                                 payload.chunks.size(), FileCrypto::sha256Hex("other")),
             IObjectStorage::FinalizeStatus::ChecksumMismatch);
    QVERIFY(!m_storage->isFinalized(key));
    // 校验失败不得破坏已收分片：用正确摘要仍可完成
    QCOMPARE(m_storage->receivedChunks(key).size(), payload.chunks.size());
    QCOMPARE(m_storage->finalize(key, payload.totalSize, MinChunkSize,
                                 payload.chunks.size(), payload.sha256Hex),
             IObjectStorage::FinalizeStatus::Ok);
}

void TestObjectStorage::finalizeIsIdempotentAfterSuccess()
{
    const ChunkedPayload payload = makePayload(2, 777, 'w');
    const QString key = m_storage->allocateBlobKey();
    for (int i = 0; i < payload.chunks.size(); ++i) {
        QVERIFY(m_storage->putChunk(key, i, payload.chunks.at(i)));
    }
    QCOMPARE(m_storage->finalize(key, payload.totalSize, MinChunkSize,
                                 payload.chunks.size(), payload.sha256Hex),
             IObjectStorage::FinalizeStatus::Ok);

    // 完成请求重试：分片已被回收，仍须回 Ok 而不是"分片缺失"
    QVERIFY(m_storage->receivedChunks(key).isEmpty());
    QCOMPARE(m_storage->finalize(key, payload.totalSize, MinChunkSize,
                                 payload.chunks.size(), payload.sha256Hex),
             IObjectStorage::FinalizeStatus::Ok);
    QCOMPARE(m_storage->blobSize(key), payload.totalSize);
}

void TestObjectStorage::finalizeRejectsInvalidChunking()
{
    const QString key = m_storage->allocateBlobKey();
    const QString sha = FileCrypto::sha256Hex("x");
    using FS = IObjectStorage::FinalizeStatus;

    QCOMPARE(m_storage->finalize(key, 0, MinChunkSize, 1, sha), FS::InvalidArguments);
    QCOMPARE(m_storage->finalize(key, -1, MinChunkSize, 1, sha), FS::InvalidArguments);
    QCOMPARE(m_storage->finalize(key, MinChunkSize, MinChunkSize - 1, 1, sha), FS::InvalidArguments);
    QCOMPARE(m_storage->finalize(key, MinChunkSize, MaxChunkSize + 1, 1, sha), FS::InvalidArguments);
    // 分片数与体积不自洽
    QCOMPARE(m_storage->finalize(key, MinChunkSize, MinChunkSize, 2, sha), FS::InvalidArguments);
    QCOMPARE(m_storage->finalize(key, 2 * MinChunkSize, MinChunkSize, 1, sha), FS::InvalidArguments);
    // 校验和形态非法（大写/长度不符）
    QCOMPARE(m_storage->finalize(key, MinChunkSize, MinChunkSize, 1, sha.toUpper()),
             FS::InvalidArguments);
    QCOMPARE(m_storage->finalize(key, MinChunkSize, MinChunkSize, 1, "abc"), FS::InvalidArguments);
    QVERIFY(!m_storage->isFinalized(key));
}

void TestObjectStorage::readRangeServesPartialContent()
{
    const ChunkedPayload payload = makePayload(2, 4321, 'v');
    const QString key = m_storage->allocateBlobKey();
    for (int i = 0; i < payload.chunks.size(); ++i) {
        QVERIFY(m_storage->putChunk(key, i, payload.chunks.at(i)));
    }
    QCOMPARE(m_storage->finalize(key, payload.totalSize, MinChunkSize,
                                 payload.chunks.size(), payload.sha256Hex),
             IObjectStorage::FinalizeStatus::Ok);

    const QByteArray all = payload.assembled();
    QCOMPARE(m_storage->readRange(key, 0, 10), all.left(10));
    QCOMPARE(m_storage->readRange(key, 100, 50), all.mid(100, 50));
    QCOMPARE(m_storage->readRange(key, 0, payload.totalSize), all);
    // 尾部越界按可用量截断（HTTP Range 的标准语义）
    QCOMPARE(m_storage->readRange(key, payload.totalSize - 10, 1000), all.right(10));
    // 起点越界与非法参数返回空
    QVERIFY(m_storage->readRange(key, payload.totalSize, 10).isEmpty());
    QVERIFY(m_storage->readRange(key, payload.totalSize + 1, 10).isEmpty());
    QVERIFY(m_storage->readRange(key, -1, 10).isEmpty());
    QVERIFY(m_storage->readRange(key, 0, 0).isEmpty());
    QVERIFY(m_storage->readRange(key, 0, -5).isEmpty());
    // 未组装的 blob 不可读（避免把半成品当完整对象投递）
    const QString other = m_storage->allocateBlobKey();
    QVERIFY(m_storage->putChunk(other, 0, payload.chunks.at(0)));
    QVERIFY(m_storage->readRange(other, 0, 10).isEmpty());
}

void TestObjectStorage::removeDeletesPartsAndBlobIdempotently()
{
    const ChunkedPayload payload = makePayload(2, 555, 'u');

    // 上传中取消：清掉分片
    const QString uploading = m_storage->allocateBlobKey();
    QVERIFY(m_storage->putChunk(uploading, 0, payload.chunks.at(0)));
    QVERIFY(m_storage->remove(uploading));
    QVERIFY(m_storage->receivedChunks(uploading).isEmpty());
    QVERIFY(!m_storage->isFinalized(uploading));
    // 幂等：再删一次仍返回成功（取消与回收任务可无条件调用）
    QVERIFY(m_storage->remove(uploading));

    // 已就绪：清掉最终对象
    const QString ready = m_storage->allocateBlobKey();
    for (int i = 0; i < payload.chunks.size(); ++i) {
        QVERIFY(m_storage->putChunk(ready, i, payload.chunks.at(i)));
    }
    QCOMPARE(m_storage->finalize(ready, payload.totalSize, MinChunkSize,
                                 payload.chunks.size(), payload.sha256Hex),
             IObjectStorage::FinalizeStatus::Ok);
    QVERIFY(m_storage->isFinalized(ready));
    QVERIFY(m_storage->remove(ready));
    QVERIFY(!m_storage->isFinalized(ready));
    QCOMPARE(m_storage->blobSize(ready), qint64(-1));
    QVERIFY(m_storage->readRange(ready, 0, 10).isEmpty());

    // 从未创建过的键也算成功
    QVERIFY(m_storage->remove(m_storage->allocateBlobKey()));
}

void TestObjectStorage::removeCleansStaleTmpFilesFromCrashedRun()
{
    // 模拟上一次组装中途崩溃留下的临时文件：名带随机后缀，
    // remove 必须按前缀枚举而非按固定名删除，否则这类残留永远清不掉
    const QString key = m_storage->allocateBlobKey();
    const QString tmpDir = m_root + "/tmp/" + key.left(2);
    QVERIFY(QDir().mkpath(tmpDir));
    for (const QString &name : {key + ".deadbeef.tmp", key + ".00000001.tmp"}) {
        QFile file(tmpDir + "/" + name);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("partial"), qint64(7));
        file.close();
    }
    QVERIFY(m_storage->putChunk(key, 0, QByteArray(64, 'a')));
    QCOMPARE(QDir(tmpDir).entryList(QStringList{key + ".*"}, QDir::Files).size(),
             qsizetype(2));

    QVERIFY(m_storage->remove(key));
    // 分片与全部临时文件一并清空
    QVERIFY(QDir(tmpDir).entryList(QStringList{key + ".*"}, QDir::Files).isEmpty());
    QVERIFY(m_storage->receivedChunks(key).isEmpty());
    QVERIFY(!m_storage->isFinalized(key));
}

QTEST_GUILESS_MAIN(TestObjectStorage)
#include "TestObjectStorage.moc"
