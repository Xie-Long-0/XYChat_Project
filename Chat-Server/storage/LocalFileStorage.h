#pragma once

#include <QDir>
#include <QMutex>
#include <QString>
#include <QStringList>

#include "IObjectStorage.h"

namespace XYChat::Server
{

// M8: 基于本地文件系统的对象存储实现
//
// 目录布局（root 由构造方传入；blobKey 前两位十六进制作分片目录，把单目录项数
// 摊平到 256 个桶，避免长期运行后单目录过大）：
//   <root>/parts/<b0b1>/<blobKey>/<index>.part  上传中的密文分片
//   <root>/tmp/<b0b1>/<blobKey>.<rand>.tmp      组装中间产物（成功后改名进 blobs）
//   <root>/blobs/<b0b1>/<blobKey>.bin           组装完成、可供下载的最终对象
//
// 所有写入都是"临时文件 + 原子改名"：进程崩溃或磁盘写满不会留下被当作完整数据的
// 半截文件。blobKey 一律由本层生成（16 字节随机数的十六进制），每次访问前重新校验
// 形态，用户可控字符串永不参与路径拼接。
//
// 并发：本实例由各连接线程共享，同一 blobKey 的 finalize/remove 经条带锁串行；
// putChunk 不入锁（QSaveFile 已保证单片原子覆盖，而并发写入与组装交叠只会让
// finalize 的长度/摘要关卡判失败，不会把损坏对象推上 blobs）。
class LocalFileStorage : public IObjectStorage
{
public:
    explicit LocalFileStorage(const QString &rootPath);

    // 存储根目录（诊断与日志用）
    QString rootPath() const { return m_root.path(); }

    // 建出目录骨架；失败返回 false，调用方应据此拒绝启动文件服务
    bool initialize();

    // IObjectStorage
    QString allocateBlobKey() override;
    bool isValidBlobKey(const QString &blobKey) const override;
    bool putChunk(const QString &blobKey, int index, const QByteArray &data) override;
    QList<int> receivedChunks(const QString &blobKey) override;
    QByteArray readChunk(const QString &blobKey, int index) override;
    FinalizeStatus finalize(const QString &blobKey, qint64 cipherSize, qint64 chunkSize,
                            int chunkCount, const QString &expectedSha256Hex) override;
    bool isFinalized(const QString &blobKey) override;
    qint64 blobSize(const QString &blobKey) override;
    QByteArray readRange(const QString &blobKey, qint64 offset, qint64 length) override;
    bool remove(const QString &blobKey) override;

    // 存储键长度：16 字节随机数的十六进制表示
    static constexpr int BlobKeyLength = 32;
    // 存储键随机源字节数
    static constexpr int BlobKeyBytes = 16;

private:
    QString partsDirPath(const QString &blobKey) const;
    QString chunkFilePath(const QString &blobKey, int index) const;
    QString tmpDirPath(const QString &blobKey) const;
    // 本次组装专用的临时文件：名中带随机后缀，即使两次组装意外并发也不会
    // 互相覆写。随机数失败时返回空串，由 finalize 归为 StorageError
    QString newTmpFilePath(const QString &blobKey) const;
    // 该 blob 遗留的全部临时文件（含崩溃残留），供 remove 清理
    QStringList existingTmpFiles(const QString &blobKey) const;
    QString blobDirPath(const QString &blobKey) const;
    QString blobFilePath(const QString &blobKey) const;
    bool ensureDir(const QString &path) const;
    // 按 blobKey 取模选条带锁
    QMutex &stripeFor(const QString &blobKey);

    QDir m_root;

    // 同一 blobKey 的 finalize/remove 必须串行：两条线程同时组装会因交错写入
    // 产出损坏对象，一条组装而另一条删除则产出"元数据 ready 而对象缺失"的
    // 不可自愈状态。用固定条带锁而非 per-key 映射：锁表不会无界增长，
    // 也不需要条目回收；条带数远大于并发上传数时误伤概率可忽略
    static constexpr int LockStripes = 64;
    QMutex m_stripes[LockStripes];
};

} // namespace XYChat::Server
