#pragma once

#include <QQuickImageProvider>

namespace XYChat::Client
{

class FileTransferManager;

// M8.3: 应用内图片查看器的图像源（QML 里写作 image://xyfile/<messageId>）
//
// 密文缓存里存的是分片独立 AEAD 加密的字节，QML 无法直接使用，必须由 C++
// 逐片解密后在内存中解码：明文只存在于返回的 QImage 里，**不落盘**（与
// "磁盘上不存在可读明文"的口径一致）。
//
// 线程模型：QQuickImageProvider::requestImage 在 Qt Quick 的渲染/读取线程执行，
// 不在 GUI 线程。因此本类只经 FileTransferManager 的线程安全接口取数据
// （manifestFor 加锁拷贝清单，IO 与解密在锁外），不直接触碰任何容器。
class FileImageProvider : public QQuickImageProvider
{
public:
    explicit FileImageProvider(FileTransferManager *transfer);

    // id 为 messageId 的十进制串。未登记清单、未下载、超过内存解码上限或
    // GCM 认证失败时返回空 QImage（QML 侧 status 变为 Error，不留半截图）
    QImage requestImage(const QString &id, QSize *size,
                        const QSize &requestedSize) override;

private:
    FileTransferManager *m_transfer;
};

} // namespace XYChat::Client
