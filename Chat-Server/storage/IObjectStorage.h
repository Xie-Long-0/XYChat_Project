#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QtGlobal>

namespace XYChat::Server
{

// M8: 对象存储抽象
//
// 上层（RequestHandler 控制面、后续独立 HTTP(S) 数据面）只依赖本接口。M8 以本地
// 文件系统实现（LocalFileStorage），后续可替换为 S3/MinIO 等外部对象存储而不改动
// 业务代码。
//
// 隐私边界：本层存取的字节一律是客户端加密后的密文（分片或组装结果），不做任何
// 解密与内容解析；文件名、MIME 等敏感元数据不出现在存储键与目录结构中。
//
// 分片口径与 Protocol::isChunkingValid 一致：chunkSize 指密文分片大小（含 16 字节
// GCM 标签），非末片恒为 chunkSize 字节，末片为余量。
class IObjectStorage
{
public:
    // finalize 的结果分类。调用方据此区分"该重传哪几片"与"整体放弃"，
    // 也决定回哪个错误码：把磁盘写满报成校验和错误，会让客户端永远重传
    enum class FinalizeStatus
    {
        Ok,                // 组装完成且整体校验通过
        InvalidArguments,  // 分片参数或校验和形态非法
        Incomplete,        // 分片缺失（receivedChunks 可给出已收索引）
        ChunkSizeMismatch, // 某分片字节数与声明不符
        ChecksumMismatch,  // 分片齐备且长度正确，但整体 SHA-256 不符
        StorageError,      // 磁盘读写/改名失败（与数据正确性无关）
    };

    virtual ~IObjectStorage() = default;

    // 为新 blob 分配不透明存储键：实现方生成的定长十六进制串，不含任何用户可控
    // 成分（文件名/用户名），从源头排除路径穿越。分配失败返回空串
    virtual QString allocateBlobKey() = 0;

    // 存储键是否为本地生成的合法形态（供上层在持久化前二次校验）
    virtual bool isValidBlobKey(const QString &blobKey) const = 0;

    // 写入一个密文分片（index 从 0 起）。重复写入同一分片为幂等覆盖，
    // 以支持客户端重试；写入采用临时文件加原子改名，并发写不产生半截分片
    virtual bool putChunk(const QString &blobKey, int index, const QByteArray &data) = 0;

    // 已落盘的分片索引（升序）。存储键非法或尚无分片时返回空列表
    virtual QList<int> receivedChunks(const QString &blobKey) = 0;

    // 读取一个分片；不存在或读失败返回空。密文分片至少含 16 字节认证标签，
    // 因此空返回值可无歧义地表示"没有这个分片"
    virtual QByteArray readChunk(const QString &blobKey, int index) = 0;

    // 校验分片齐备后组装为最终对象，并核对整体 SHA-256（小写 hex）。
    // 仅当返回 Ok 时才产出最终对象；其余结果均保留已收分片供重传
    // （fail-closed：宁可要求重传，不得让损坏对象进入 ready）。
    // 组装为流式过程，不把整个文件读进内存
    virtual FinalizeStatus finalize(const QString &blobKey, qint64 cipherSize, qint64 chunkSize,
                                    int chunkCount, const QString &expectedSha256Hex) = 0;

    // 最终对象是否已就绪（组装成功且可供下载）
    virtual bool isFinalized(const QString &blobKey) = 0;

    // 最终对象大小（字节）；未就绪或不存在返回 -1
    virtual qint64 blobSize(const QString &blobKey) = 0;

    // 读取最终对象的一段（供 HTTP Range 下载与断点续下）。
    // 参数越界、未就绪或读失败返回空
    virtual QByteArray readRange(const QString &blobKey, qint64 offset, qint64 length) = 0;

    // 删除 blob 的分片、临时文件与最终对象。不存在也返回 true（幂等），
    // 以便取消上传与过期回收可以无条件调用
    virtual bool remove(const QString &blobKey) = 0;
};

} // namespace XYChat::Server
