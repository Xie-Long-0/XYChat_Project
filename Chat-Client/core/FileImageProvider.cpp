#include "FileImageProvider.h"

#include <QByteArray>
#include <QImage>
#include <QSize>

#include "FileTransferManager.h"

namespace XYChat::Client
{

FileImageProvider::FileImageProvider(FileTransferManager *transfer)
    : QQuickImageProvider(QQmlImageProviderBase::Image)
    , m_transfer(transfer)
{
}

QImage FileImageProvider::requestImage(const QString &id, QSize *size,
                                       const QSize &requestedSize)
{
    QImage image;
    if (!m_transfer) {
        return image;
    }

    bool ok = false;
    const qint64 messageId = id.toLongLong(&ok);
    if (!ok || messageId <= 0) {
        return image;
    }

    // 解密在引擎内完成（清单加锁拷贝、IO 与 GCM 认证在锁外）；返回空表示
    // 未登记清单、未下载、超过内存解码上限或缓存损坏
    const QByteArray bytes = m_transfer->decryptedFileBytes(messageId);
    if (bytes.isEmpty()) {
        return image;
    }
    if (!image.loadFromData(bytes)) {
        return image;  // 内容不是可解码的图片格式（例如把 PDF 当图片预览）
    }
    if (size) {
        *size = image.size();
    }

    // QML 的 sourceSize 会以 requestedSize 传入；未指定的维度为 -1
    if (requestedSize.width() > 0 && requestedSize.height() > 0) {
        if (image.size() != requestedSize) {
            image = image.scaled(requestedSize, Qt::KeepAspectRatio,
                                 Qt::SmoothTransformation);
        }
    } else if (requestedSize.width() > 0 && image.width() > requestedSize.width()) {
        image = image.scaledToWidth(requestedSize.width(), Qt::SmoothTransformation);
    } else if (requestedSize.height() > 0 && image.height() > requestedSize.height()) {
        image = image.scaledToHeight(requestedSize.height(), Qt::SmoothTransformation);
    }
    return image;
}

} // namespace XYChat::Client
