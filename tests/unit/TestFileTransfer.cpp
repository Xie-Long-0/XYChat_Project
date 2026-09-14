// M8.2: 客户端文件传输引擎端到端集成测试
//
// 起真实的 HTTP 数据面（FileHttpService）+ 真实的本地对象存储，控制面由测试
// 内的迷你实现顶替（直接操作 DatabaseManager 与 IObjectStorage，等价于
// RequestHandler 的行为），从而在不启动完整服务端进程的前提下验证：
//   明文 -> 分片加密 -> HTTP 上传 -> 服务端组装 -> HTTP 下载 -> 逐片解密
//   -> 保存到本地，全流程字节级一致；且服务端全程只见密文。
#include <QtTest>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QImageWriter>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QTemporaryDir>
#include <QTimer>

#include <memory>

#include "core/FileTransferManager.h"
#include "core/DecryptingIODevice.h"
#include "database/DatabaseManager.h"
#include "http/FileHttpService.h"
#include "storage/LocalFileStorage.h"

#include "FileCrypto.h"
#include "FileProtocol.h"

using namespace XYChat::Client;
using namespace XYChat::Server;
using XYChat::Security::FileCrypto;
namespace Protocol = XYChat::Protocol;

class TestFileTransfer : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void uploadThenDownloadRestoresBytesExactly();
    void serverOnlyEverSeesCiphertext();
    void uploadedManifestIsSelfContained();
    void cachedFileSkipsSecondDownload();
    void downloadRejectsServerMetadataMismatch();
    void saveRefusesWithoutDownloadOrManifest();
    void clearCacheRemovesBytesAndFlipsState();
    void disabledWithoutBaseUrl();
    void resetClearsRegisteredManifests();
    // M8.3: 图片上传后清单必须带上尺寸与内联缩略图（接收方靠它在下载
    // 原图之前就能展示预览），而非图片文件不得携带缩略图
    void imageUploadCarriesThumbnailMetadata();
    // 两个任务并存且其中一个在调度循环内同步失败（源文件被删）：
    // 失败路径会 erase 任务节点，旧实现直接在 QHash 上迭代会使当前
    // 迭代器失效（UB），并因 finishTask/failTask 递归调 pumpNext 而嵌套泵送
    void taskFailingDuringPumpKeepsSchedulerConsistent();
    // M8.3c: QML 侧无可靠的 url→本地路径手段（全局 Qt 对象并无 urlToLocalFile），
    // 因此引擎必须自己吃下 file:// URL。这条用例覆盖 QUrl/字符串/UNC/纯本地路径/
    // 非 file 协议/空值六种输入形态，确保 uploadAndSend 与 saveToFile 拿到正确路径
    void toLocalPathAcceptsUrlsAndPlainPaths();
    // M8.3c: 应用内图片查看器的图像源经 decryptedFileBytes 取整份明文（内存解码，
    // 不落盘）。必须验：登记+缓存命中时逐字节还原、未登记/未下载/超上限时返回空
    void decryptedFileBytesRestoresPlainForRegisteredMessage();
    // M8.3b: 播放器经 DecryptingIODevice 从密文缓存流式逐片解密（明文不落盘）。
    // 必须验：顺序读取逐字节还原、seek 后读取正确、未下载/未登记时 open 失败
    void decryptingIODeviceRestoresPlainBytes();
    void decryptingIODeviceSupportsSeek();
    void decryptingIODeviceFailsWithoutCache();

private:
    // 迷你控制面：与 RequestHandler 的口径一致（创建即签上传票据、完成即
    // 组装并转 ready、下载前签短时效票据）
    void wireControlPlane(FileTransferManager &engine);
    bool waitForTask(FileTransferManager &engine, const QString &token, int timeoutMs = 30000);
    bool writeSourceFile(const QString &path, qint64 sizeBytes);
    QByteArray readFile(const QString &path);
    // 取一个已上传完成的 fileId（前置断言非空）：用例可被单独运行，
    // 不得假定前一个用例已填充过映射
    qint64 requireUploadedFile();

    QTemporaryDir *m_dir = nullptr;
    QString m_sourcePath;      // 上传源文件（明文）
    QString m_storageRoot;     // 服务端对象存储根
    QString m_cacheRoot;       // 客户端密文缓存根
    QString m_savedPath;       // 保存目标

    std::unique_ptr<LocalFileStorage> m_storage;
    std::unique_ptr<FileHttpService> m_http;
    DatabaseManager *m_db = nullptr;
    QString m_connName;
    qint64 m_userId = 0;
    QHash<qint64, QString> m_blobKeys;   // fileId -> blobKey（迷你控制面用）
    qint64 m_sourceSize = 0;
    // 首个用例产出的清单（含其密钥）。每文件独立密钥使相同明文产生
    // 不同密文，因此缓存复用类用例必须沿用同一份清单，不能重新上传
    QString m_firstManifestJson;
};

void TestFileTransfer::initTestCase()
{
    m_dir = new QTemporaryDir();
    QVERIFY(m_dir->isValid());
    m_storageRoot = m_dir->path() + "/server-storage";
    m_cacheRoot = m_dir->path() + "/client-cache";
    m_sourcePath = m_dir->path() + "/source.bin";
    m_savedPath = m_dir->path() + "/saved.bin";

    m_storage = std::make_unique<LocalFileStorage>(m_storageRoot);
    QVERIFY(m_storage->initialize());

    m_connName = "test_file_transfer";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", m_connName);
        db.setDatabaseName(":memory:");
        QVERIFY(db.open());
    }
    m_db = new DatabaseManager(m_connName);
    QVERIFY(m_db->initialize());
    m_userId = m_db->registerUser("ftuser", QString(), QString(), "v1:1:salt:hash");
    QVERIFY(m_userId > 0);

    m_http = std::make_unique<FileHttpService>(nullptr, m_connName);
    m_http->setObjectStorage(m_storage.get());
    QVERIFY(m_http->start(0, QSslConfiguration(), false, true));

    // 2.5 MiB 明文：按 1 MiB 密文分片会切成 3 片（末片为余量），
    // 足以覆盖"多片 + 末片余量"这两条最容易出错的路径
    m_sourceSize = 2 * 1024 * 1024 + 512 * 1024;
    QVERIFY(writeSourceFile(m_sourcePath, m_sourceSize));
}

void TestFileTransfer::cleanupTestCase()
{
    m_http.reset();
    delete m_db;
    m_db = nullptr;
    {
        QSqlDatabase db = QSqlDatabase::database(m_connName, false);
        if (db.isOpen()) {
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(m_connName);
    m_storage.reset();
    delete m_dir;
    m_dir = nullptr;
}

bool TestFileTransfer::writeSourceFile(const QString &path, qint64 sizeBytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    // 可辨识的伪随机内容：全零或恒定字节会让"分片错位/漏片"这类缺陷隐形
    QByteArray block(64 * 1024, '\0');
    qint64 written = 0;
    quint32 state = 0x9E3779B9u;
    while (written < sizeBytes) {
        for (int i = 0; i < block.size(); ++i) {
            state = state * 1664525u + 1013904223u;
            block[i] = static_cast<char>((state >> 24) & 0xFF);
        }
        const qint64 want = qMin(static_cast<qint64>(block.size()), sizeBytes - written);
        if (file.write(block.left(want)) != want) {
            return false;
        }
        written += want;
    }
    file.close();
    return true;
}

QByteArray TestFileTransfer::readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QByteArray data = file.readAll();
    file.close();
    return data;
}

qint64 TestFileTransfer::requireUploadedFile()
{
    // QHash 无序且 Qt 6 不提供 firstKey；本处只要求"任一个已上传的文件"。
    // 空映射意味着前置用例未运行：返回 -1 让调用处既有的
    // QVERIFY(rec.has_value()) 安全失败，而不是解引用 end 迭代器
    if (m_blobKeys.isEmpty()) {
        return -1;
    }
    return m_blobKeys.begin().key();
}

void TestFileTransfer::wireControlPlane(FileTransferManager &engine)
{
    connect(&engine, &FileTransferManager::uploadCreateRequested, this,
            [this, &engine](qint64 seq, qint64 cipherSize, qint64 chunkSize, int chunkCount,
                            const QString &sha256Hex) {
                const QString blobKey = m_storage->allocateBlobKey();
                const qint64 fileId = m_db->createFileRecord(m_userId, "deviceA", blobKey,
                                                             cipherSize, chunkSize, chunkCount,
                                                             sha256Hex);
                if (fileId <= 0) {
                    engine.onUploadCreated(seq, false, 0, QString(), "create failed");
                    return;
                }
                const QString ticket = FileCrypto::generateTicket();
                m_db->issueFileTicket(fileId, m_userId, Protocol::FileTicketKind::Upload,
                                      FileCrypto::ticketHash(ticket), 3600);
                m_blobKeys.insert(fileId, blobKey);
                engine.onUploadCreated(seq, true, fileId, ticket, QString());
            });

    connect(&engine, &FileTransferManager::uploadQueryRequested, this,
            [this, &engine](qint64 seq, qint64 fileId) {
                QList<int> received;
                const auto rec = m_db->getFileRecord(fileId);
                if (rec.has_value()) {
                    received = m_storage->receivedChunks(rec->blobKey);
                }
                engine.onUploadQueried(seq, rec.has_value(), received, QString());
            });

    connect(&engine, &FileTransferManager::uploadCompleteRequested, this,
            [this, &engine](qint64 seq, qint64 fileId) {
                const auto rec = m_db->getFileRecord(fileId);
                if (!rec.has_value()) {
                    engine.onUploadCompleted(seq, false, "no such file");
                    return;
                }
                const auto status = m_storage->finalize(rec->blobKey, rec->sizeBytes,
                                                        rec->chunkSize, rec->chunkCount,
                                                        rec->sha256Hex);
                const bool ok = status == IObjectStorage::FinalizeStatus::Ok
                    && m_db->markFileReady(fileId);
                // 与控制面一致：上传终结即吊销上传票据
                m_db->revokeFileTickets(fileId, Protocol::FileTicketKind::Upload);
                engine.onUploadCompleted(seq, ok, ok ? QString() : QString("finalize failed"));
            });

    connect(&engine, &FileTransferManager::uploadCancelRequested, this,
            [this, &engine](qint64 seq, qint64 fileId) {
                const auto rec = m_db->getFileRecord(fileId);
                if (rec.has_value()) {
                    m_db->markFileCancelled(fileId);
                    m_storage->remove(rec->blobKey);
                }
                engine.onUploadCancelled(seq, true, QString());
            });

    connect(&engine, &FileTransferManager::downloadTicketRequested, this,
            [this, &engine](qint64 seq, qint64 fileId) {
                const auto rec = m_db->getFileRecord(fileId);
                if (!rec.has_value() || rec->status != Protocol::FileStatus::Ready) {
                    engine.onDownloadTicket(seq, false, QString(), 0, 0, 0, QString(), "not ready");
                    return;
                }
                const QString ticket = FileCrypto::generateTicket();
                m_db->issueFileTicket(fileId, m_userId, Protocol::FileTicketKind::Download,
                                      FileCrypto::ticketHash(ticket), 300);
                engine.onDownloadTicket(seq, true, ticket, rec->sizeBytes, rec->chunkSize,
                                        rec->chunkCount, rec->sha256Hex, QString());
            });
}

bool TestFileTransfer::waitForTask(FileTransferManager &engine, const QString &token,
                                   int timeoutMs)
{
    QEventLoop loop;
    bool succeeded = false;
    auto finishedConn = connect(&engine, &FileTransferManager::taskFinished, &loop,
                                [&loop, &succeeded, token](const QString &t) {
                                    if (t == token) {
                                        succeeded = true;
                                        loop.quit();
                                    }
                                });
    auto failedConn = connect(&engine, &FileTransferManager::taskFailed, &loop,
                              [&loop, &succeeded, token](const QString &t, const QString &) {
                                  if (t == token) {
                                      succeeded = false;
                                      loop.quit();
                                  }
                              });
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    loop.exec();
    disconnect(finishedConn);
    disconnect(failedConn);
    return succeeded;
}

void TestFileTransfer::uploadThenDownloadRestoresBytesExactly()
{
    FileTransferManager engine;
    engine.setCacheRoot(m_cacheRoot);
    engine.setBaseUrl(m_http->baseUrl());
    wireControlPlane(engine);

    QSignalSpy sendSpy(&engine, &FileTransferManager::sendMessageRequested);
    const QString uploadToken = engine.uploadAndSend(m_sourcePath, 0, 7);
    QVERIFY(!uploadToken.isEmpty());
    QVERIFY2(waitForTask(engine, uploadToken), "upload did not finish");
    QCOMPARE(sendSpy.count(), 1);

    // 服务端已把文件标为 ready，且元数据与本地口径一致
    const qint64 fileId = sendSpy.at(0).at(3).toLongLong();
    QVERIFY(fileId > 0);
    const auto rec = m_db->getFileRecord(fileId);
    QVERIFY(rec.has_value());
    QCOMPARE(rec->status, QString("ready"));
    QVERIFY(rec->sizeBytes > m_sourceSize);  // 每片各多一个 GCM 标签

    // 登记清单（模拟接收方解密消息后的动作）并下载
    const QString manifestJson = sendSpy.at(0).at(2).toString();
    m_firstManifestJson = manifestJson;
    const qint64 messageId = 4242;
    engine.registerIncomingFile(messageId, manifestJson);
    QVERIFY(!engine.isMessageFileAvailable(messageId));  // 尚未下载

    QSignalSpy stateSpy(&engine, &FileTransferManager::downloadStateChanged);
    const QString downloadToken = engine.download(messageId);
    QVERIFY2(waitForTask(engine, downloadToken), "download did not finish");
    QVERIFY(engine.isMessageFileAvailable(messageId));
    QVERIFY(stateSpy.count() >= 1);

    // 保存并逐字节比对：这是整条链路唯一的最终判据
    QVERIFY(engine.saveToFile(messageId, m_savedPath));
    const QByteArray original = readFile(m_sourcePath);
    const QByteArray saved = readFile(m_savedPath);
    QCOMPARE(saved.size(), original.size());
    QCOMPARE(saved, original);
}

void TestFileTransfer::serverOnlyEverSeesCiphertext()
{
    // 复用上一用例已上传的文件：直接检查服务端 blob 的内容
    const qint64 fileId = requireUploadedFile();
    const QString blobKey = m_blobKeys.value(fileId);
    const auto rec = m_db->getFileRecord(fileId);
    QVERIFY(rec.has_value());

    const QByteArray blob = m_storage->readRange(blobKey, 0, rec->sizeBytes);
    QCOMPARE(static_cast<qint64>(blob.size()), rec->sizeBytes);

    const QByteArray plain = readFile(m_sourcePath);
    // 服务端 blob 不得与明文有任何前缀/整体相同，也不得包含明文的开头片段：
    // 分片是独立 AEAD 密文，任何明文残留都意味着加密环节被跳过
    QVERIFY(blob != plain);
    QVERIFY(!blob.startsWith(plain.left(4096)));
    QVERIFY(!blob.contains(plain.left(1024)));
    // 服务端元数据里也不得出现文件名（它只在客户端清单内）
    QVERIFY(!rec->blobKey.contains("source"));
}

void TestFileTransfer::uploadedManifestIsSelfContained()
{
    FileTransferManager engine;
    engine.setCacheRoot(m_cacheRoot);
    engine.setBaseUrl(m_http->baseUrl());
    wireControlPlane(engine);

    QSignalSpy sendSpy(&engine, &FileTransferManager::sendMessageRequested);
    const QString manifestToken = engine.uploadAndSend(m_sourcePath, 12, 0);
    QVERIFY2(waitForTask(engine, manifestToken), "upload did not finish");
    QCOMPARE(sendSpy.count(), 1);
    const QString manifestJson = sendSpy.at(0).at(2).toString();

    // 清单必须自包含：仅凭它（与服务端 blob）就能解密，不依赖服务端声明的口径
    bool ok = false;
    const Protocol::FileManifest manifest = Protocol::decodeFileManifest(manifestJson, &ok);
    QVERIFY(ok);
    QVERIFY(manifest.isValid());
    QCOMPARE(manifest.plainSize, m_sourceSize);
    QCOMPARE(manifest.chunkSize, Protocol::DefaultChunkSize);
    QCOMPARE(manifest.key.size(), 32);
    QCOMPARE(manifest.iv.size(), 12);
    QVERIFY(Protocol::isChunkingValid(manifest.cipherSize, manifest.chunkSize,
                                      Protocol::chunkCountFor(manifest.cipherSize,
                                                              manifest.chunkSize)));
    // 文件名与 MIME 随清单走 E2EE，服务端不可见
    QCOMPARE(manifest.name, QString("source.bin"));
    QVERIFY(!manifest.mime.isEmpty());
    // 非图片文件不得携带缩略图与尺寸（字段留空而不是塞垃圾）
    QVERIFY(manifest.thumbnail.isEmpty());
    QCOMPARE(manifest.width, 0);
    QCOMPARE(manifest.height, 0);
}

void TestFileTransfer::imageUploadCarriesThumbnailMetadata()
{
    // 用 PNG（QtGui 内建编码器，无需插件）造一张测试图片
    const QString imagePath = m_dir->path() + "/photo.png";
    QImage image(640, 480, QImage::Format_RGB32);
    image.fill(QColor(200, 80, 60));
    QVERIFY(image.save(imagePath, "PNG"));

    FileTransferManager engine;
    engine.setCacheRoot(m_cacheRoot);
    engine.setBaseUrl(m_http->baseUrl());
    wireControlPlane(engine);

    QSignalSpy sendSpy(&engine, &FileTransferManager::sendMessageRequested);
    const QString token = engine.uploadAndSend(imagePath, 5, 0);
    QVERIFY2(waitForTask(engine, token), "image upload did not finish");
    QCOMPARE(sendSpy.count(), 1);

    bool ok = false;
    const Protocol::FileManifest manifest =
        Protocol::decodeFileManifest(sendSpy.at(0).at(2).toString(), &ok);
    QVERIFY(ok);
    QVERIFY(manifest.isValid());
    // 尺寸与原图一致
    QCOMPARE(manifest.width, 640);
    QCOMPARE(manifest.height, 480);
    QCOMPARE(manifest.name, QString("photo.png"));

    if (QImageWriter::supportedImageFormats().contains("jpeg")) {
        QVERIFY(!manifest.thumbnail.isEmpty());
        QVERIFY(manifest.thumbnail.size() <= Protocol::MaxThumbnailBytes);
        // 缩略图必须是可解码的 JPEG（否则 UI 侧 data URL 会渲染失败）
        QImage thumb;
        QVERIFY(thumb.loadFromData(manifest.thumbnail, "JPEG"));
        QVERIFY(!thumb.isNull());
        QVERIFY(thumb.width() <= 160 && thumb.height() <= 160);
    }

    // 清单整体（含 base64 缩略图）仍受群消息正文长度上限约束，
    // 否则图片消息会被服务端以超长为由拒收
    QVERIFY(Protocol::encodeFileManifest(manifest).size() < 16384 / 2);
}

void TestFileTransfer::cachedFileSkipsSecondDownload()
{
    // 沿用首个用例的清单（同一份密文）。重新上传同一文件不会命中缓存：
    // 每文件独立密钥使相同明文产生不同密文，这是 E2EE 的固有性质
    QVERIFY(!m_firstManifestJson.isEmpty());
    bool ok = false;
    const Protocol::FileManifest manifest =
        Protocol::decodeFileManifest(m_firstManifestJson, &ok);
    QVERIFY(ok);
    QVERIFY(manifest.isValid());

    FileTransferManager engine;
    engine.setCacheRoot(m_cacheRoot);
    engine.setBaseUrl(m_http->baseUrl());
    wireControlPlane(engine);

    QVERIFY(engine.isCached(manifest.sha256Hex, manifest.cipherSize));
    const qint64 messageId = 99;
    engine.registerIncomingFile(messageId, m_firstManifestJson);
    QVERIFY(engine.isMessageFileAvailable(messageId));

    // 把基地址改成不可路由的地址：若仍成功，证明确实走了缓存而非网络
    engine.setBaseUrl("http://127.0.0.1:1/file");
    QSignalSpy finishedSpy(&engine, &FileTransferManager::taskProgress);
    QSignalSpy doneSpy(&engine, &FileTransferManager::taskFinished);
    QSignalSpy failSpy(&engine, &FileTransferManager::taskFailed);
    const QString token = engine.download(messageId);
    // 缓存命中时任务在调用期间同步完成（不发任何 HTTP 请求），
    // 因此不能用"先调用后等信号"的写法，否则会白等到超时
    QCOMPARE(doneSpy.count(), 1);
    QCOMPARE(doneSpy.at(0).at(0).toString(), token);
    QCOMPARE(failSpy.count(), 0);
    bool sawCachedPhase = false;
    for (const auto &args : finishedSpy) {
        if (args.at(1).toString() == QLatin1String("cached")) {
            sawCachedPhase = true;
        }
    }
    QVERIFY(sawCachedPhase);

    // 命中缓存后仍可正确保存
    const QString second = m_dir->path() + "/saved-from-cache.bin";
    QVERIFY(engine.saveToFile(messageId, second));
    QCOMPARE(readFile(second), readFile(m_sourcePath));
}

void TestFileTransfer::downloadRejectsServerMetadataMismatch()
{
    FileTransferManager engine;
    engine.setCacheRoot(m_cacheRoot);
    engine.setBaseUrl(m_http->baseUrl());

    // 手工构造一份清单，其 sha256 与服务端记录不符（等价于服务端换了 blob
    // 或记录被篡改）：引擎必须拒绝，而不是下载完再靠 GCM 失败发现
    const qint64 fileId = requireUploadedFile();
    const auto rec = m_db->getFileRecord(fileId);
    QVERIFY(rec.has_value());

    const auto key = FileCrypto::generateFileKey();
    Protocol::FileManifest manifest;
    manifest.fileId = fileId;
    manifest.name = "tampered.bin";
    manifest.mime = "application/octet-stream";
    manifest.plainSize = m_sourceSize;
    manifest.cipherSize = rec->sizeBytes;
    manifest.chunkSize = rec->chunkSize;
    manifest.sha256Hex = FileCrypto::sha256Hex("a-different-blob");
    manifest.key = key.key;
    manifest.iv = key.iv;
    const QString manifestJson = Protocol::encodeFileManifest(manifest);
    QVERIFY(!manifestJson.isEmpty());

    // 迷你控制面：故意回服务端真实记录（与清单不一致）
    connect(&engine, &FileTransferManager::downloadTicketRequested, this,
            [this, &engine](qint64 seq, qint64 id) {
                const auto record = m_db->getFileRecord(id);
                QVERIFY(record.has_value());
                const QString ticket = FileCrypto::generateTicket();
                m_db->issueFileTicket(id, m_userId, Protocol::FileTicketKind::Download,
                                      FileCrypto::ticketHash(ticket), 300);
                engine.onDownloadTicket(seq, true, ticket, record->sizeBytes, record->chunkSize,
                                        record->chunkCount, record->sha256Hex, QString());
            });

    const qint64 messageId = 555;
    engine.registerIncomingFile(messageId, manifestJson);
    QVERIFY(!engine.isMessageFileAvailable(messageId));

    QSignalSpy failSpy(&engine, &FileTransferManager::taskFailed);
    const QString downloadToken = engine.download(messageId);
    QVERIFY(!downloadToken.isEmpty());
    // 元数据不符在票据回调里同步判定，任务在调用期间即已失败
    QCOMPARE(failSpy.count(), 1);
    QCOMPARE(failSpy.at(0).at(0).toString(), downloadToken);
    QVERIFY(failSpy.at(0).at(1).toString().contains("does not match the manifest"));
    // 失败后不得留下半截缓存
    QVERIFY(!engine.isCached(manifest.sha256Hex, manifest.cipherSize));
}

void TestFileTransfer::saveRefusesWithoutDownloadOrManifest()
{
    FileTransferManager engine;
    engine.setCacheRoot(m_cacheRoot);
    engine.setBaseUrl(m_http->baseUrl());

    // 未登记清单的消息：拒绝保存（不得凭空造文件）
    QVERIFY(!engine.saveToFile(123456, m_dir->path() + "/nope.bin"));
    // 空目标路径：拒绝
    const qint64 fileId = requireUploadedFile();
    const auto rec = m_db->getFileRecord(fileId);
    QVERIFY(rec.has_value());

    // 已登记但未下载（清单 sha256 与缓存内容无关）：拒绝并提示需先下载
    const auto key = FileCrypto::generateFileKey();
    Protocol::FileManifest manifest;
    manifest.fileId = fileId;
    manifest.name = "not-downloaded.bin";
    manifest.mime = "application/octet-stream";
    manifest.plainSize = 1024;
    manifest.cipherSize = 1040;
    manifest.chunkSize = Protocol::MinChunkSize;
    manifest.sha256Hex = FileCrypto::sha256Hex("never-downloaded");
    manifest.key = key.key;
    manifest.iv = key.iv;
    const QString json = Protocol::encodeFileManifest(manifest);
    QVERIFY(!json.isEmpty());
    engine.registerIncomingFile(777, json);
    QVERIFY(!engine.isMessageFileAvailable(777));
    QVERIFY(!engine.saveToFile(777, m_dir->path() + "/nope2.bin"));
}

void TestFileTransfer::clearCacheRemovesBytesAndFlipsState()
{
    QVERIFY(!m_firstManifestJson.isEmpty());
    FileTransferManager engine;
    engine.setCacheRoot(m_cacheRoot);
    engine.setBaseUrl(m_http->baseUrl());
    wireControlPlane(engine);
    QVERIFY(engine.cacheBytes() > 0);

    // 用首个用例的清单登记：缓存里就是这份密文，因此立即可用
    engine.registerIncomingFile(888, m_firstManifestJson);
    QVERIFY(engine.isMessageFileAvailable(888));

    QSignalSpy stateSpy(&engine, &FileTransferManager::downloadStateChanged);
    const int removed = engine.clearCache();
    QVERIFY(removed >= 1);
    QCOMPARE(engine.cacheBytes(), qint64(0));
    // 清理后状态回到"缺失"，且保存被拒（缓存只是副本，可重新下载）
    QVERIFY(!engine.isMessageFileAvailable(888));
    QVERIFY(!engine.saveToFile(888, m_dir->path() + "/after-clear.bin"));
    bool sawMissing = false;
    for (const auto &args : stateSpy) {
        if (args.at(1).toString() == QLatin1String("missing")) {
            sawMissing = true;
        }
    }
    QVERIFY(sawMissing);
}

void TestFileTransfer::disabledWithoutBaseUrl()
{
    FileTransferManager engine;
    engine.setCacheRoot(m_cacheRoot);
    QVERIFY(!engine.isEnabled());

    // 服务端未开启文件能力（登录响应无 fileTransferBaseUrl）：任务立即失败，
    // 而不是静默排队让用户以为在上传
    QSignalSpy failSpy(&engine, &FileTransferManager::taskFailed);
    const QString token = engine.uploadAndSend(m_sourcePath, 0, 3);
    QVERIFY(!token.isEmpty());
    QCOMPARE(failSpy.count(), 1);
    QCOMPARE(failSpy.at(0).at(0).toString(), token);
    QVERIFY(failSpy.at(0).at(1).toString().contains("not available"));
    QCOMPARE(engine.activeTaskCount(), 0);

    // 下载同样立即失败
    const QString downloadToken = engine.download(1);
    QVERIFY(!downloadToken.isEmpty());
    QCOMPARE(failSpy.count(), 2);
}

void TestFileTransfer::resetClearsRegisteredManifests()
{
    FileTransferManager engine;
    engine.setCacheRoot(m_cacheRoot);
    engine.setBaseUrl(m_http->baseUrl());
    // 上传要走控制面，未接线会使任务永远停在 creating 阶段
    wireControlPlane(engine);

    // 沿用首个用例的清单：本用例要验的是"已登记且可用的清单被 reset 清掉"，
    // 而不是再传一份新文件（新密钥会产生新密文，与缓存无关）
    QVERIFY(!m_firstManifestJson.isEmpty());
    engine.registerIncomingFile(999, m_firstManifestJson);
    // 缓存是否命中取决于前序用例（clearCache 可能已清空），故缺失时先下载一次，
    // 避免用例之间产生隐式顺序耦合
    if (!engine.isMessageFileAvailable(999)) {
        const QString downloadToken = engine.download(999);
        QVERIFY2(waitForTask(engine, downloadToken), "download did not finish");
    }
    QVERIFY(engine.isMessageFileAvailable(999));
    QVERIFY(engine.saveToFile(999, m_dir->path() + "/before-reset.bin"));

    // 登出/断线：清单含文件密钥，必须清零且不得跨会话驻留
    engine.reset();
    QVERIFY(!engine.isMessageFileAvailable(999));
    QVERIFY(!engine.saveToFile(999, m_dir->path() + "/after-reset.bin"));
    QCOMPARE(engine.activeTaskCount(), 0);
}

void TestFileTransfer::taskFailingDuringPumpKeepsSchedulerConsistent()
{
    const QString vanishingPath = m_dir->path() + "/vanishing.bin";
    QVERIFY(writeSourceFile(vanishingPath, 256 * 1024));

    FileTransferManager engine;
    engine.setCacheRoot(m_cacheRoot);
    engine.setBaseUrl(m_http->baseUrl());
    wireControlPlane(engine);

    QSignalSpy doneSpy(&engine, &FileTransferManager::taskFinished);
    QSignalSpy failSpy(&engine, &FileTransferManager::taskFailed);

    // 第一个任务先占住传输槽（串行），第二个任务排队
    const QString first = engine.uploadAndSend(m_sourcePath, 0, 21);
    const QString second = engine.uploadAndSend(vanishingPath, 0, 22);
    QVERIFY(!first.isEmpty());
    QVERIFY(!second.isEmpty());
    QCOMPARE(engine.activeTaskCount(), 2);

    // 删掉第二个任务的源文件：它在被泵送时（第一个任务完成之后）才会
    // 发现文件打不开，从而在调度循环内部同步失败
    QVERIFY(QFile::remove(vanishingPath));

    QVERIFY2(waitForTask(engine, first), "first upload did not finish");
    // 第二个任务必须已明确失败（而不是让调度器崩溃或把它永久留在表里）
    QTRY_VERIFY_WITH_TIMEOUT(failSpy.count() >= 1, 10000);
    bool sawSecondFailure = false;
    for (const auto &args : failSpy) {
        if (args.at(0).toString() == second) {
            sawSecondFailure = true;
        }
    }
    QVERIFY(sawSecondFailure);
    // token 必须是可读的真实字符串：若 emit 时引用已悬垂（节点已 erase），
    // 这里会是空串、乱码或直接崩溃（Id128 形式固定 32 位十六进制）
    QCOMPARE(doneSpy.at(0).at(0).toString().length(), 32);
    QCOMPARE(failSpy.at(0).at(0).toString().length(), 32);
    QCOMPARE(engine.activeTaskCount(), 0);
}

void TestFileTransfer::toLocalPathAcceptsUrlsAndPlainPaths()
{
    FileTransferManager engine;
    // 未设基地址不影响纯函数行为（toLocalPath 不走网络）

    // ① QML 的 FileDialog.selectedFile 以 QUrl 形态传入（signal 参数用 var
    // 保留）：QVariant 内部 typeId 为 QMetaType::QUrl，引擎直接取 toLocalFile
    const QUrl fileUrl = QUrl::fromLocalFile(m_sourcePath);
    QCOMPARE(engine.toLocalPath(fileUrl), m_sourcePath);

    // ② 以字符串形态传入的 file:// URL：走 QUrl(text).toLocalFile() 分支
    QCOMPARE(engine.toLocalPath(fileUrl.toString()), m_sourcePath);

    // ③ UNC 路径：正则剔前缀对 file://server/share/x 会错，
    // QUrl::toLocalFile 会正确还原为 \\server\share\x（Windows 上）
    const QUrl uncUrl("file://server/share/doc.txt");
    const QString unc = engine.toLocalPath(uncUrl);
    QVERIFY(!unc.isEmpty());
    QVERIFY(unc.contains("server"));
    QVERIFY(unc.contains("share"));
    QVERIFY(unc.contains("doc.txt"));

    // ④ 已是本地路径（QML 也可能直接传字符串）：原样返回
    QCOMPARE(engine.toLocalPath(m_sourcePath), m_sourcePath);

    // ⑤ 非 file 协议的 URL：返回空串（不得当成路径拿去打开）
    QVERIFY(engine.toLocalPath(QUrl("https://example.com/x.png")).isEmpty());
    QVERIFY(engine.toLocalPath(QString("https://example.com/x.png")).isEmpty());

    // ⑥ 空值与空白：返回空串（避免 QFileInfo("").exists() 之类的无意义查询）
    QVERIFY(engine.toLocalPath(QVariant()).isEmpty());
    QVERIFY(engine.toLocalPath(QString("")).isEmpty());
    QVERIFY(engine.toLocalPath(QString("   ")).isEmpty());

    // ⑦ Windows 盘符路径（C:/... 与 C:\...）：冒号前只有一个字母时是盘符
    // 而不是 URL scheme，必须原样返回（否则在 Windows 上会把所有绝对路径都误判为 URL）
    QCOMPARE(engine.toLocalPath(QString("C:/Users/test/photo.png")),
             QString("C:/Users/test/photo.png"));
    QCOMPARE(engine.toLocalPath(QString("C:\\Users\\test\\photo.png")),
             QString("C:\\Users\\test\\photo.png"));
}

void TestFileTransfer::decryptedFileBytesRestoresPlainForRegisteredMessage()
{
    FileTransferManager engine;
    engine.setCacheRoot(m_cacheRoot);
    engine.setBaseUrl(m_http->baseUrl());
    wireControlPlane(engine);

    // 未登记清单：不得凭空返回字节（否则会绕过下载流程与 GCM 校验）
    QVERIFY(engine.decryptedFileBytes(1234567).isEmpty());

    // 登记一份未下载的清单（用随机摘要，确保缓存中不存在）：
    // 即使登记了也不得返回字节，否则大图预览会读到旧文件或垃圾数据
    const auto key = FileCrypto::generateFileKey();
    Protocol::FileManifest missing;
    missing.fileId = 1;
    missing.name = "never-downloaded.bin";
    missing.mime = "application/octet-stream";
    missing.plainSize = 4096;
    missing.cipherSize = 4112;
    missing.chunkSize = Protocol::MinChunkSize;
    missing.sha256Hex = FileCrypto::sha256Hex("never-downloaded-bytes");
    missing.key = key.key;
    missing.iv = key.iv;
    const QString missingJson = Protocol::encodeFileManifest(missing);
    QVERIFY(!missingJson.isEmpty());
    engine.registerIncomingFile(31337, missingJson);
    QVERIFY(!engine.isMessageFileAvailable(31337));
    QVERIFY(engine.decryptedFileBytes(31337).isEmpty());

    // 走完整上传→登记→下载链路，再断言 decryptedFileBytes 逐字节还原
    QSignalSpy sendSpy(&engine, &FileTransferManager::sendMessageRequested);
    const QString uploadToken = engine.uploadAndSend(m_sourcePath, 0, 42);
    QVERIFY2(waitForTask(engine, uploadToken), "upload did not finish");
    QCOMPARE(sendSpy.count(), 1);
    const qint64 messageId = 424242;
    engine.registerIncomingFile(messageId, sendSpy.at(0).at(2).toString());
    const QString downloadToken = engine.download(messageId);
    QVERIFY2(waitForTask(engine, downloadToken), "download did not finish");
    QVERIFY(engine.isMessageFileAvailable(messageId));

    const QByteArray plain = engine.decryptedFileBytes(messageId);
    const QByteArray original = readFile(m_sourcePath);
    QCOMPARE(plain.size(), original.size());
    QCOMPARE(plain, original);

    // 超过内存解码上限的清单（伪造 plainSize > MaxInMemoryDecodeBytes）：
    // 引擎必须直接返回空，而不是把整份明文读进内存把进程打爆
    Protocol::FileManifest huge = missing;
    huge.plainSize = FileTransferManager::MaxInMemoryDecodeBytes + 1;
    huge.cipherSize = huge.plainSize + 16;
    huge.sha256Hex = FileCrypto::sha256Hex("huge-manifest");
    const QString hugeJson = Protocol::encodeFileManifest(huge);
    QVERIFY(!hugeJson.isEmpty());
    engine.registerIncomingFile(999999, hugeJson);
    QVERIFY(engine.decryptedFileBytes(999999).isEmpty());
}

void TestFileTransfer::decryptingIODeviceRestoresPlainBytes()
{
    // 沿用首个用例的清单（缓存里就是这份密文）；若被前序用例清空则先下载
    QVERIFY(!m_firstManifestJson.isEmpty());
    FileTransferManager engine;
    engine.setCacheRoot(m_cacheRoot);
    engine.setBaseUrl(m_http->baseUrl());
    wireControlPlane(engine);

    const qint64 messageId = 5150;
    engine.registerIncomingFile(messageId, m_firstManifestJson);
    if (!engine.isMessageFileAvailable(messageId)) {
        const QString downloadToken = engine.download(messageId);
        QVERIFY2(waitForTask(engine, downloadToken), "download did not finish");
    }
    QVERIFY(engine.isMessageFileAvailable(messageId));

    // DecryptingIODevice 从密文缓存流式逐片解密：顺序读取应逐字节还原明文
    DecryptingIODevice device(messageId, &engine);
    QVERIFY(device.open(QIODevice::ReadOnly));
    QCOMPARE(device.size(), m_sourceSize);
    QVERIFY(!device.isSequential());
    const QByteArray plain = device.readAll();
    device.close();
    QCOMPARE(static_cast<qint64>(plain.size()), m_sourceSize);
    QCOMPARE(plain, readFile(m_sourcePath));
}

void TestFileTransfer::decryptingIODeviceSupportsSeek()
{
    QVERIFY(!m_firstManifestJson.isEmpty());
    FileTransferManager engine;
    engine.setCacheRoot(m_cacheRoot);
    engine.setBaseUrl(m_http->baseUrl());
    wireControlPlane(engine);

    const qint64 messageId = 5151;
    engine.registerIncomingFile(messageId, m_firstManifestJson);
    if (!engine.isMessageFileAvailable(messageId)) {
        const QString downloadToken = engine.download(messageId);
        QVERIFY2(waitForTask(engine, downloadToken), "download did not finish");
    }

    const QByteArray original = readFile(m_sourcePath);
    DecryptingIODevice device(messageId, &engine);
    QVERIFY(device.open(QIODevice::ReadOnly));

    // seek 到中间偏移读取后半段：分片是独立 AEAD，seek 后必须能定位到
    // 正确分片并解密（播放器拖动进度条依赖此）
    const qint64 mid = m_sourceSize / 2;
    QVERIFY(device.seek(mid));
    QCOMPARE(device.pos(), mid);
    const QByteArray tail = device.read(m_sourceSize - mid);
    QCOMPARE(tail, original.mid(static_cast<int>(mid)));

    // seek 回开头再读：验证可重复定位
    QVERIFY(device.seek(0));
    const QByteArray head = device.read(1024);
    QCOMPARE(head, original.left(1024));

    // seek 到跨分片边界的非对齐位置：读取跨越两个分片，验证解密拼接正确
    const qint64 plainChunk = Protocol::plainSizeOfChunk(Protocol::DefaultChunkSize);
    if (m_sourceSize > plainChunk + 200) {
        const qint64 crossBoundary = plainChunk - 50;
        QVERIFY(device.seek(crossBoundary));
        const QByteArray segment = device.read(200);
        QCOMPARE(segment, original.mid(static_cast<int>(crossBoundary), 200));
    }
    device.close();
}

void TestFileTransfer::decryptingIODeviceFailsWithoutCache()
{
    FileTransferManager engine;
    engine.setCacheRoot(m_cacheRoot);
    engine.setBaseUrl(m_http->baseUrl());

    // 未登记清单：open 失败（不得凭空造数据）
    DecryptingIODevice noManifest(999999, &engine);
    QVERIFY(!noManifest.open(QIODevice::ReadOnly));

    // 登记一份未下载的清单（随机摘要，缓存中不存在）：open 失败
    const auto key = FileCrypto::generateFileKey();
    Protocol::FileManifest missing;
    missing.fileId = 1;
    missing.name = "never-downloaded.bin";
    missing.mime = "application/octet-stream";
    missing.plainSize = 4096;
    missing.cipherSize = 4112;
    missing.chunkSize = Protocol::MinChunkSize;
    missing.sha256Hex = FileCrypto::sha256Hex("never-downloaded-for-device");
    missing.key = key.key;
    missing.iv = key.iv;
    const QString missingJson = Protocol::encodeFileManifest(missing);
    QVERIFY(!missingJson.isEmpty());
    engine.registerIncomingFile(31338, missingJson);
    QVERIFY(!engine.isMessageFileAvailable(31338));
    DecryptingIODevice noCache(31338, &engine);
    QVERIFY(!noCache.open(QIODevice::ReadOnly));
}

QTEST_GUILESS_MAIN(TestFileTransfer)
#include "TestFileTransfer.moc"
