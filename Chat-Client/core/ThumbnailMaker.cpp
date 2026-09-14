#include "ThumbnailMaker.h"

#include <QBuffer>
#include <QImage>
#include <QImageReader>
#include <QSize>

namespace XYChat::Client
{

QByteArray ThumbnailMaker::encodeThumbnail(const QImage &image, int maxEdge, int maxBytes)
{
    if (image.isNull() || maxEdge <= 0 || maxBytes <= 0) {
        return {};
    }

    // 先缩放到最长边 maxEdge（image 已经 <= maxEdge 时不放大，避免无谓插值）：
    // 视频封面帧可能是原生分辨率（如 1920x1080），直接压缩既慢又难压进 4 KiB
    QImage candidate = image;
    if (candidate.width() > maxEdge || candidate.height() > maxEdge) {
        candidate = candidate.scaled(maxEdge, maxEdge, Qt::KeepAspectRatio,
                                     Qt::SmoothTransformation);
        if (candidate.isNull()) {
            return {};
        }
    }

    // 逐步降质量，质量到底仍超限再折半降尺寸：照片内容在 160px/质量 70 下
    // 仍可能超过 4 KiB，而清单体积受群消息正文长度上限硬约束
    const int qualities[] = {70, 55, 40, 25, 15};
    int edge = qMax(candidate.width(), candidate.height());
    for (int attempt = 0; attempt < 9; ++attempt) {
        const int quality = qualities[qMin(attempt, 4)];
        QByteArray encoded;
        {
            QBuffer buffer(&encoded);
            if (!buffer.open(QIODevice::WriteOnly)) {
                break;
            }
            if (!candidate.save(&buffer, "JPEG", quality)) {
                break;  // 编码器不可用：再试也无益
            }
        }
        if (encoded.size() <= maxBytes) {
            return encoded;
        }
        // 质量已降到最低仍超限：折半缩小尺寸后重来（下限 32px，再小无意义）
        if (attempt >= 4) {
            edge = qMax(32, edge / 2);
            candidate = candidate.scaled(edge, edge, Qt::KeepAspectRatio,
                                         Qt::SmoothTransformation);
            if (candidate.isNull()) {
                break;
            }
        }
    }
    // 压不进上限就不内联缩略图（UI 回退到文件图标 + 文件名）。绝不放宽上限：
    // 清单撑破群消息正文长度会让整条文件消息被服务端拒绝
    return {};
}

ThumbnailMaker::Result ThumbnailMaker::make(const QString &filePath, int maxEdge, int maxBytes)
{
    Result result;
    if (filePath.isEmpty() || maxEdge <= 0 || maxBytes <= 0) {
        return result;
    }

    QImageReader reader(filePath);
    // EXIF 方向必须校正：否则手机竖拍的照片会得到横向的缩略图
    reader.setAutoTransform(true);
    const QSize original = reader.size();
    if (!original.isValid() || original.width() <= 0 || original.height() <= 0) {
        // 不是图片，或容器里读不出尺寸（读不出尺寸就无法安全地限制解码开销）
        return result;
    }
    result.isImage = true;

    // 先设缩放尺寸再解码：JPEG 等格式可利用缩放解码，避免把原图完整读进内存
    //（一张 50 MP 的照片解码后约 200 MB）
    QSize scaled = original;
    scaled.scale(maxEdge, maxEdge, Qt::KeepAspectRatio);
    if (!scaled.isValid() || scaled.width() <= 0 || scaled.height() <= 0) {
        scaled = QSize(maxEdge, maxEdge);
    }
    reader.setScaledSize(scaled);

    const QImage image = reader.read();
    if (image.isNull()) {
        // 尺寸可读但解码失败（文件损坏或编码不受支持）：仍上报尺寸，缩略图留空
        result.width = original.width();
        result.height = original.height();
        return result;
    }

    // EXIF 旋转 90/270 度会交换宽高：以解码后的朝向为准修正上报尺寸，
    // 否则 UI 显示的宽高比与用户实际看到的图不一致
    const bool swapped = (image.width() > image.height()) != (scaled.width() > scaled.height());
    result.width = swapped ? original.height() : original.width();
    result.height = swapped ? original.width() : original.height();

    // 压缩逻辑复用 encodeThumbnail：image 已经是缩放后的（约 maxEdge），
    // encodeThumbnail 内部的再缩放为 no-op，行为与重构前一致
    result.thumb = encodeThumbnail(image, maxEdge, maxBytes);
    return result;
}

} // namespace XYChat::Client
