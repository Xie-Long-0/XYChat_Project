#include "MediaPlaybackManager.h"

#include <QAudioOutput>
#include <QMediaPlayer>
#include <QUrl>
#include <QVideoSink>

#include "DecryptingIODevice.h"
#include "FileTransferManager.h"

namespace XYChat::Client
{

MediaPlaybackManager::MediaPlaybackManager(FileTransferManager *transfer, QObject *parent)
    : QObject(parent)
    , m_transfer(transfer)
    , m_player(new QMediaPlayer(this))
    , m_audioOutput(new QAudioOutput(this))
    , m_videoSink(new QVideoSink(this))
{
    m_player->setAudioOutput(m_audioOutput);
    // 视频帧由 QVideoSink 接收（即使 QML 当前不渲染动态画面，也需关联 sink，
    // 否则某些后端在播放含视频轨的文件时会报错）
    m_player->setVideoSink(m_videoSink);

    connect(m_player, &QMediaPlayer::playbackStateChanged, this,
            [this](QMediaPlayer::PlaybackState) { emit playbackStateChanged(); });
    connect(m_player, &QMediaPlayer::positionChanged, this,
            [this](qint64) { emit positionChanged(); });
    connect(m_player, &QMediaPlayer::durationChanged, this,
            [this](qint64) { emit durationChanged(); });
    connect(m_player, &QMediaPlayer::hasVideoChanged, this, [this](bool hasVideo) {
        m_hasVideo = hasVideo;
        emit mediaInfoChanged();
    });
    connect(m_player, &QMediaPlayer::errorOccurred, this,
            [this](QMediaPlayer::Error, const QString &errorString) {
                emit errorOccurred(errorString);
            });
    connect(m_audioOutput, &QAudioOutput::volumeChanged, this,
            [this](float) { emit volumeChanged(); });
}

MediaPlaybackManager::~MediaPlaybackManager()
{
    stop();
}

int MediaPlaybackManager::playbackState() const
{
    switch (m_player->playbackState()) {
    case QMediaPlayer::PlayingState:
        return Playing;
    case QMediaPlayer::PausedState:
        return Paused;
    default:
        return Stopped;
    }
}

qint64 MediaPlaybackManager::position() const
{
    return m_player->position();
}

qint64 MediaPlaybackManager::duration() const
{
    return m_player->duration();
}

int MediaPlaybackManager::volume() const
{
    return static_cast<int>(m_audioOutput->volume() * 100.0f);
}

bool MediaPlaybackManager::hasVideo() const
{
    return m_hasVideo;
}

bool MediaPlaybackManager::active() const
{
    return m_device != nullptr;
}

bool MediaPlaybackManager::play(qint64 messageId)
{
    if (!m_transfer || messageId <= 0) {
        return false;
    }
    // 切换媒体前先停止并清理当前设备（QMediaPlayer 一次只持有一个 sourceDevice）
    stop();

    auto *device = new DecryptingIODevice(messageId, m_transfer, this);
    if (!device->open(QIODevice::ReadOnly)) {
        device->deleteLater();
        emit errorOccurred(
            "Cannot open the file for playback (not downloaded, not media, or cache corrupt)");
        return false;
    }
    m_device = device;
    m_currentMessageId = messageId;

    // setSourceDevice 的 QUrl 参数用于标识流类型；密文缓存解密后是裸的音视频
    // 字节流（mp3/mp4 等带魔数），平台后端会从数据探测格式，故用伪 URL 即可
    m_player->setSourceDevice(m_device, QUrl("xychat-stream://media"));
    emit activeChanged();
    m_player->play();
    return true;
}

void MediaPlaybackManager::pause()
{
    if (m_device) {
        m_player->pause();
    }
}

void MediaPlaybackManager::resume()
{
    if (m_device) {
        m_player->play();
    }
}

void MediaPlaybackManager::stop()
{
    m_player->stop();
    teardownDevice();
}

void MediaPlaybackManager::setPosition(qint64 pos)
{
    if (m_device) {
        m_player->setPosition(pos);
    }
}

void MediaPlaybackManager::setVolume(int volume)
{
    m_audioOutput->setVolume(qBound(0, volume, 100) / 100.0f);
}

void MediaPlaybackManager::teardownDevice()
{
    if (!m_device) {
        return;
    }
    // 先解绑设备再关闭：QMediaPlayer 持有 device 指针，若先 delete 会悬垂
    m_player->setSourceDevice(nullptr, QUrl());
    m_device->close();
    m_device->deleteLater();
    m_device = nullptr;
    m_currentMessageId = 0;
    m_hasVideo = false;
    emit mediaInfoChanged();
    emit activeChanged();
}

} // namespace XYChat::Client
