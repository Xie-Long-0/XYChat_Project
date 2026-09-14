#pragma once

#include <QByteArray>
#include <QFile>
#include <QIODevice>
#include <QMutex>

#include "FileProtocol.h"

namespace XYChat::Client
{

class FileTransferManager;

// M8.3b: 解密 QIODevice（供 QMediaPlayer 播放密文缓存中的音视频）
//
// 从密文缓存逐片解密后喂给 QMediaPlayer，明文只在内存中流转、不落盘
//（与 FileImageProvider 的"明文不落盘"口径一致）。QMediaPlayer::setSourceDevice
// 接受 QIODevice*，本类实现 readData/size/seek 以支持顺序读取与拖动进度条。
//
// 线程模型：QMediaPlayer 可能在媒体线程读取本设备。本类在 open() 时从
// FileTransferManager 获取清单副本（playbackInfoForMessage 内部加锁），之后
// 不再访问 FileTransferManager，因此内部无需加锁（QMediaPlayer 对单个
// sourceDevice 的读取是串行的）。
class DecryptingIODevice : public QIODevice
{
    Q_OBJECT

public:
    // 构造时不打开文件；调用方需显式 open(ReadOnly)。
    // messageId 用于从 FileTransferManager 查清单（含密钥）与密文缓存路径
    DecryptingIODevice(qint64 messageId, FileTransferManager *transfer,
                       QObject *parent = nullptr);
    ~DecryptingIODevice() override;

    bool isSequential() const override { return false; }
    qint64 size() const override;
    qint64 bytesAvailable() const override;
    // open/close/seek 在 QIODevice 中是 public virtual，必须保持 public
    //（MediaPlaybackManager 需调用 open/close，播放器拖动进度条需 seek）
    bool open(OpenMode mode) override;
    void close() override;
    bool seek(qint64 pos) override;

protected:
    qint64 readData(char *data, qint64 maxlen) override;
    qint64 writeData(const char *data, qint64 len) override;

private:
    // 确保 m_chunkBuffer 包含 plainPos 所在的片，并将 m_chunkBufferOffset
    // 指向 plainPos 在缓冲内的位置。返回是否成功（false 表示缓存损坏或越界）
    bool ensureChunkFor(qint64 plainPos);

    qint64 m_messageId;
    FileTransferManager *m_transfer;
    XYChat::Protocol::FileManifest m_manifest;
    QFile m_cacheFile;
    bool m_opened = false;

    // 保护下列可变状态与 m_cacheFile/m_opened/m_manifest：Qt6 在 Windows 上默认
    // 走 FFmpeg 后端，readData/seek 由 demuxer 工作线程调用，而 open/close 在
    // GUI 线程。不加锁则 teardown（GUI 线程 close+wipe）会与在途 readData
    //（后端线程读 m_chunkBuffer）竞争，造成对已 wipe/clear 缓冲的读取甚至 UAF
    mutable QMutex m_mutex;

    // 当前明文位置（与 QIODevice::pos() 一致）
    qint64 m_plainPos = 0;
    // 当前片的明文缓冲与缓冲内偏移
    QByteArray m_chunkBuffer;
    qint64 m_chunkBufferOffset = 0;
    // 当前片对应的明文起始位置（-1 表示缓冲无效，需重新解密）
    qint64 m_chunkPlainStart = -1;
    int m_chunkCount = 0;
    qint64 m_plainChunkSize = 0;  // chunkSize - GCM 标签
};

} // namespace XYChat::Client
