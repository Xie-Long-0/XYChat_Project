#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

class QMediaPlayer;
class QVideoSink;
class QTimer;

namespace XYChat::Client
{

// M8.3b: 音视频元数据异步提取器
//
// 用 QMediaPlayer + QVideoSink 异步加载音视频文件，提取时长、分辨率与视频封面。
// 平台解码后端（Windows 上 Media Foundation）不可用、文件非音视频、或超时护栏
// 触发时，emit finished(Result{ok=false})，调用方据此留空元数据（与图片路径
// "元数据缺失绝不阻断发送"口径一致）。
//
// 线程模型：本类必须在 GUI 线程创建与使用（QMediaPlayer 要求 GUI 线程，且
// QVideoSink 的 videoFrame 只在 GUI 线程有效）。异步提取期间调用方不应销毁
// 本对象；提取完成或超时后 emit 信号，调用方再清理。
//
// 提取流程：
//   1. setSource(QUrl::fromLocalFile(path)) + setVideoSink(sink)
//   2. 等待 mediaStatusChanged 到 LoadedMedia/BufferedMedia
//   3. 读取 duration() 与 metaData().value(Resolution)
//   4. 若有有效分辨率（视频），seek 到 min(1000, duration/2) 毫秒抓封面帧
//   5. 等待 videoFrameChanged，从 frame.toImage() 获取 QImage，encodeThumbnail 压缩
//   6. emit finished(Result)
//   音频（无分辨率）在步骤 3 后直接 emit finished（thumbnail 为空）。
class MediaMetadataExtractor : public QObject
{
    Q_OBJECT

public:
    struct Result
    {
        bool ok = false;         // 提取是否成功（超时/错误/非音视频为 false）
        qint64 durationMs = 0;   // 时长（毫秒），未知为 0
        int width = 0;           // 视频分辨率宽，音频/未知为 0
        int height = 0;          // 视频分辨率高
        QByteArray thumbnail;    // JPEG 视频封面；音频/失败/压不进上限为空
    };

    explicit MediaMetadataExtractor(QObject *parent = nullptr);
    ~MediaMetadataExtractor() override;

    // 开始提取。timeoutMs 为超时护栏（默认 5 秒），超时则 emit finished(Result{ok=false})。
    // 同一时间只提取一个文件；重复调用会先取消前一个（不 emit 信号）。
    void extract(const QString &filePath, int timeoutMs = 5000);

    // 取消当前提取（不 emit 信号）
    void cancel();

signals:
    void finished(const Result &result);

private:
    // mediaStatus 到达 LoadedMedia/BufferedMedia 后调用：读取 duration 与分辨率，
    // 视频则 seek 抓帧，音频则直接 finish
    void onMetadataLoaded();
    // videoFrameChanged 后调用：从 frame 抓封面并 finish
    void onVideoFrameReady();
    void finishWith(const Result &result);
    void cleanup();

    QMediaPlayer *m_player = nullptr;
    QVideoSink *m_videoSink = nullptr;
    QTimer *m_timeoutTimer = nullptr;
    QString m_currentPath;
    bool m_seekedForThumbnail = false;
    bool m_finished = false;
    // 临时存储：mediaStatus 到达后读取的元数据，等封面抓帧完成后一起 emit
    qint64 m_durationMs = 0;
    int m_width = 0;
    int m_height = 0;
    bool m_isVideo = false;  // 是否有有效分辨率（视频）
    // 提取代次：每次 extract 自增。信号 lambda 捕获发起时的代次，处理时
    // 与当前 m_generation 比对，不一致则丢弃（cancel/切换媒体后后端可能有
    // 排队的陈旧信号经事件循环到达，不丢弃会串扰下一次提取）
    int m_generation = 0;
};

} // namespace XYChat::Client
