#pragma once

#include <QByteArray>
#include <QString>

#include "FileProtocol.h"

namespace XYChat::Client
{

// M8.3: 图片元数据与内联缩略图生成
//
// 只用 QtGui 的图像编解码（QImageReader/QImage），不依赖平台多媒体后端，
// 因此在无头环境与 CI 中同样可验证。音频/视频的时长与视频封面需 QtMultimedia
// 与平台解码器（Windows 上为 Media Foundation），不在本类范围内。
//
// 缩略图内联在 FileManifest 里，随消息正文经既有 E2EE（私聊 envelope / 群聊
// Sender-Key）分发，服务端不可见。因此这里产出的是 JPEG **明文**字节：清单
// 整体已被加密，再单独加一层只会增加复杂度而无安全收益。
//
// 体积受 Protocol::MaxThumbnailBytes 约束：清单要作为群消息正文投递，受
// MaxGroupMessageLength（16384 字符）限制，base64 后约 1.34 倍膨胀。
class ThumbnailMaker
{
public:
    struct Result
    {
        int width = 0;       // 原图像素宽（未知或非图片为 0）
        int height = 0;      // 原图像素高
        QByteArray thumb;    // JPEG 缩略图字节；空表示未能生成或未压进上限
        bool isImage = false;
    };

    // 读取图片尺寸并生成不超过 maxBytes 的 JPEG 缩略图（最长边 maxEdge 像素）。
    //
    // 失败语义：非图片、读不出尺寸、解码失败或压不进上限时 thumb 为空，
    // 但 isImage/width/height 仍按已读到的信息填。调用方据此留空清单字段即可，
    // **元数据缺失绝不影响文件传输本身**（宁可不显示缩略图，也不阻断发送）。
    static Result make(const QString &filePath, int maxEdge = 160,
                       int maxBytes = XYChat::Protocol::MaxThumbnailBytes);
};

} // namespace XYChat::Client
