#pragma once

#include <QObject>
#include <QString>

class QMediaPlayer;
class QAudioOutput;
class QVideoSink;

namespace XYChat::Client
{

class FileTransferManager;
class DecryptingIODevice;

// M8.3b: 音视频播放器（C++ QMediaPlayer + DecryptingIODevice，明文不落盘）
//
// 从密文缓存流式解密喂给 QMediaPlayer 播放（setSourceDevice），明文只在内存中
// 流转。音频播放完整；视频播放输出音频轨，视频帧由 QVideoSink 接收，但 QML
// 侧当前展示静态封面（清单里的 thumb）+ 播放控制——动态画面渲染需自定义
// QSGNode/QQuickPaintedItem（QML VideoOutput 无法绑定 C++ QVideoSink），已登记
// 为欠账。
//
// 线程模型：本类在 GUI 线程创建与使用（QMediaPlayer 要求 GUI 线程）。
class MediaPlaybackManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int playbackState READ playbackState NOTIFY playbackStateChanged)
    Q_PROPERTY(qint64 position READ position NOTIFY positionChanged)
    Q_PROPERTY(qint64 duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(int volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(bool hasVideo READ hasVideo NOTIFY mediaInfoChanged)
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)

public:
    // 与 QML 绑定的播放状态（映射自 QMediaPlayer::PlaybackState）
    enum PlaybackState { Stopped = 0, Playing, Paused };
    Q_ENUM(PlaybackState)

    explicit MediaPlaybackManager(FileTransferManager *transfer, QObject *parent = nullptr);
    ~MediaPlaybackManager() override;

    int playbackState() const;
    qint64 position() const;
    qint64 duration() const;
    int volume() const;       // 0..100
    bool hasVideo() const;
    bool active() const;      // 是否有正在播放/暂停的媒体

public slots:
    // 播放某消息的音视频。返回是否成功启动（清单缺失/缓存未就绪/打开失败返回 false）。
    // 重复调用会先停止当前播放再切换到新媒体
    bool play(qint64 messageId);
    void pause();
    void resume();
    void stop();
    void setPosition(qint64 pos);
    void setVolume(int volume);

signals:
    void playbackStateChanged();
    void positionChanged();
    void durationChanged();
    void volumeChanged();
    void mediaInfoChanged();
    void activeChanged();
    void errorOccurred(QString error);

private:
    void teardownDevice();

    FileTransferManager *m_transfer;
    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_audioOutput = nullptr;
    QVideoSink *m_videoSink = nullptr;
    DecryptingIODevice *m_device = nullptr;
    qint64 m_currentMessageId = 0;
    bool m_hasVideo = false;
};

} // namespace XYChat::Client
