#include "MediaMetadataExtractor.h"

#include <QImage>
#include <QMediaMetaData>
#include <QMediaPlayer>
#include <QSize>
#include <QTimer>
#include <QUrl>
#include <QVideoFrame>
#include <QVideoSink>

#include "ThumbnailMaker.h"

namespace XYChat::Client
{

MediaMetadataExtractor::MediaMetadataExtractor(QObject *parent)
    : QObject(parent)
    , m_player(new QMediaPlayer(this))
    , m_videoSink(new QVideoSink(this))
    , m_timeoutTimer(new QTimer(this))
{
    m_timeoutTimer->setSingleShot(true);
    // 超时护栏：如果已经拿到元数据（duration/分辨率），则上报元数据但 thumbnail
    // 为空（抓帧超时或平台后端不支持 seek）；否则整个提取失败。这样即使封面
    // 抓帧失败，时长与分辨率仍能上报，UI 不至于完全回退到文件图标
    connect(m_timeoutTimer, &QTimer::timeout, this, [this]() {
        if (m_finished) {
            return;
        }
        Result result;
        if (m_durationMs > 0 || m_width > 0 || m_height > 0) {
            result.ok = true;
            result.durationMs = m_durationMs;
            result.width = m_width;
            result.height = m_height;
        }
        finishWith(result);
    });

    // setVideoSink 必须在 setSource 之前：某些平台后端在 setSource 时就会
    // 开始解码，若此时 sink 未关联则 videoFrame 不会被推送
    m_player->setVideoSink(m_videoSink);

    // mediaStatusChanged/errorOccurred/videoFrameChanged 的连接移到 extract()：
    // 每次提取重新连接并捕获当次 generation，使 cancel/切换媒体后后端排队的
    // 陈旧信号能按 generation 丢弃，不串扰下一次提取
}

MediaMetadataExtractor::~MediaMetadataExtractor()
{
    cleanup();
}

void MediaMetadataExtractor::extract(const QString &filePath, int timeoutMs)
{
    // 重复调用：先取消前一个（不 emit 信号），再开始新的提取
    cancel();

    if (filePath.isEmpty()) {
        // 空路径：同步 emit 失败（不启动 timer）。先置 m_finished 防止重入，
        // emit 后重置以便后续 extract
        m_finished = true;
        emit finished(Result{});
        m_finished = false;
        return;
    }

    m_currentPath = filePath;
    m_finished = false;
    m_seekedForThumbnail = false;
    m_durationMs = 0;
    m_width = 0;
    m_height = 0;
    m_isVideo = false;

    // 重新连接信号并捕获当次 generation：先断开旧连接避免累积。陈旧信号
    //（cancel/切换媒体后端排队的）到达时 gen != m_generation 被丢弃
    ++m_generation;
    const int gen = m_generation;
    disconnect(m_player, nullptr, this, nullptr);
    disconnect(m_videoSink, nullptr, this, nullptr);
    connect(m_player, &QMediaPlayer::mediaStatusChanged, this,
            [this, gen](QMediaPlayer::MediaStatus status) {
                if (gen != m_generation || m_finished) {
                    return;
                }
                if (status == QMediaPlayer::LoadedMedia
                    || status == QMediaPlayer::BufferedMedia) {
                    onMetadataLoaded();
                } else if (status == QMediaPlayer::InvalidMedia) {
                    // 文件不是音视频格式，或平台后端无法解码
                    finishWith(Result{});
                }
                // 其他状态（LoadingMedia/BufferingMedia/StalledMedia/EndOfMedia/NoMedia）
                // 不处理：等待 LoadedMedia/BufferedMedia 或超时护栏
            });
    connect(m_player, &QMediaPlayer::errorOccurred, this,
            [this, gen](QMediaPlayer::Error error, const QString &) {
                if (gen != m_generation || m_finished) {
                    return;
                }
                if (error != QMediaPlayer::NoError) {
                    finishWith(Result{});
                }
            });
    connect(m_videoSink, &QVideoSink::videoFrameChanged, this,
            [this, gen](const QVideoFrame &) {
                // 只在 seek 抓帧阶段处理：播放过程中的 frame 变化不触发
                if (gen != m_generation || m_finished || !m_seekedForThumbnail) {
                    return;
                }
                onVideoFrameReady();
            });

    m_timeoutTimer->start(timeoutMs > 0 ? timeoutMs : 5000);
    // setSource 后平台后端会自动加载元数据（mediaStatus: LoadingMedia -> LoadedMedia），
    // 不需要 play()（play 会开始播放，音频会出声）。某些后端可能需要 play() 才能
    // 加载元数据，若测试发现问题再调整
    m_player->setSource(QUrl::fromLocalFile(filePath));
}

void MediaMetadataExtractor::cancel()
{
    // 总是安全的：即使没有进行中的提取也调用 cleanup
    m_finished = true;  // 防止后续信号触发 finishWith
    m_timeoutTimer->stop();
    cleanup();
    m_finished = false;  // 重置，以便后续 extract
}

void MediaMetadataExtractor::onMetadataLoaded()
{
    m_durationMs = m_player->duration();

    const QMediaMetaData metaData = m_player->metaData();
    const QVariant resolutionVar = metaData.value(QMediaMetaData::Resolution);
    if (resolutionVar.isValid() && resolutionVar.canConvert<QSize>()) {
        const QSize resolution = resolutionVar.toSize();
        if (resolution.width() > 0 && resolution.height() > 0) {
            m_width = resolution.width();
            m_height = resolution.height();
            m_isVideo = true;
        }
    }

    if (m_isVideo && m_durationMs > 0) {
        // seek 到 min(1000, duration/2) 毫秒抓封面帧：开头可能是黑帧，
        // 中间帧更能代表视频内容；但长视频 seek 到中间可能慢，故上限 1 秒
        const qint64 seekPos = qMin<qint64>(1000, m_durationMs / 2);
        m_seekedForThumbnail = true;
        m_player->setPosition(seekPos);
        // 等待 videoFrameChanged；如果平台后端不支持 seek 或视频帧，
        // 主超时护栏会在剩余时间内触发，上报 duration/resolution 但 thumbnail 为空
    } else {
        // 音频（无分辨率）或无 duration：直接 finish，thumbnail 为空
        Result result;
        result.ok = true;
        result.durationMs = m_durationMs;
        result.width = m_width;
        result.height = m_height;
        finishWith(result);
    }
}

void MediaMetadataExtractor::onVideoFrameReady()
{
    const QVideoFrame frame = m_videoSink->videoFrame();
    if (!frame.isValid()) {
        return;  // 等待下一帧
    }
    const QImage image = frame.toImage();
    if (image.isNull()) {
        return;  // 等待下一帧（某些后端首帧可能无效）
    }

    // 如果 frame 的尺寸与 metaData 的 Resolution 不一致，以 frame 为准
    //（某些平台后端的 metaData Resolution 可能不准确或为空）
    if (frame.size().width() > 0 && frame.size().height() > 0) {
        m_width = frame.size().width();
        m_height = frame.size().height();
    }

    // 复用 ThumbnailMaker 的压缩逻辑（逐步降质量、折半降尺寸至 4 KiB 上限内）
    const QByteArray thumb = ThumbnailMaker::encodeThumbnail(image);

    Result result;
    result.ok = true;
    result.durationMs = m_durationMs;
    result.width = m_width;
    result.height = m_height;
    result.thumbnail = thumb;
    finishWith(result);
}

void MediaMetadataExtractor::finishWith(const Result &result)
{
    if (m_finished) {
        return;
    }
    m_finished = true;
    m_timeoutTimer->stop();
    emit finished(result);
    cleanup();
}

void MediaMetadataExtractor::cleanup()
{
    if (m_player) {
        m_player->stop();
        // 释放文件句柄：否则同一文件被占用，后续上传/删除会失败
        m_player->setSource(QUrl());
    }
    m_seekedForThumbnail = false;
    m_currentPath.clear();
}

} // namespace XYChat::Client
