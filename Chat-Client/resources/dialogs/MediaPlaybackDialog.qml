import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../theme"
import "../components"

AppDialog {
    id: root

    property int messageId: 0
    property var messageInfo: null
    readonly property bool hasPlayer: typeof mediaPlayer !== "undefined"

    title: {
        return (root.messageInfo && root.messageInfo.fileName
                && root.messageInfo.fileName.length > 0)
               ? root.messageInfo.fileName : "播放"
    }

    width: Math.min(parent.width * 0.7, 480)
    standardButtons: Dialog.NoButton

    onRejected: {
        if (hasPlayer) mediaPlayer.stop()
        root.messageId = 0
    }

    contentItem: ColumnLayout {
        spacing: Theme.spacingMedium

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 200
            radius: Theme.radiusMedium
            color: Theme.inputBackground
            clip: true

            Image {
                anchors.fill: parent
                visible: root.hasPlayer && mediaPlayer.hasVideo
                source: {
                    return (root.messageInfo && root.messageInfo.fileThumb
                            && root.messageInfo.fileThumb.length > 0)
                           ? "data:image/jpeg;base64," + root.messageInfo.fileThumb : ""
                }
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                smooth: true
            }

            Icon {
                anchors.centerIn: parent
                visible: !root.hasPlayer || !mediaPlayer.hasVideo
                name: "music"
                size: 64
                iconColor: Theme.textTertiary
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingSmall

            AppButton {
                text: (root.hasPlayer && mediaPlayer.playbackState === 1)
                      ? "暂停" : "播放"
                variant: "secondary"
                onClicked: {
                    if (!root.hasPlayer) return
                    if (mediaPlayer.playbackState === 1) mediaPlayer.pause()
                    else if (mediaPlayer.playbackState === 2) mediaPlayer.resume()
                    else if (root.messageId > 0) mediaPlayer.play(root.messageId)
                }
            }

            Label {
                text: root.formatMs(root.hasPlayer ? mediaPlayer.position : 0)
                font.pixelSize: Theme.fontSizeCaption
                color: Theme.textSecondary
            }

            Slider {
                id: positionSlider
                Layout.fillWidth: true
                from: 0
                to: (root.hasPlayer && mediaPlayer.duration > 0)
                    ? mediaPlayer.duration : 1
                enabled: root.hasPlayer && mediaPlayer.duration > 0
                onMoved: {
                    if (root.hasPlayer) mediaPlayer.setPosition(positionSlider.value)
                }
            }

            Label {
                text: root.formatMs(root.hasPlayer ? mediaPlayer.duration : 0)
                font.pixelSize: Theme.fontSizeCaption
                color: Theme.textSecondary
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingSmall

            Label {
                text: "音量"
                font.pixelSize: Theme.fontSizeCaption
                color: Theme.textSecondary
            }

            Slider {
                id: volumeSlider
                Layout.fillWidth: true
                from: 0; to: 100
                value: root.hasPlayer ? mediaPlayer.volume : 100
                onMoved: {
                    if (root.hasPlayer) mediaPlayer.setVolume(volumeSlider.value)
                }
            }
        }
    }

    footer: DialogButtonBox {
        AppButton {
            text: "关闭"
            variant: "flat"
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
        }
    }

    function formatMs(ms) {
        if (ms <= 0) return "00:00"
        var totalSeconds = Math.floor(ms / 1000)
        var hours = Math.floor(totalSeconds / 3600)
        var minutes = Math.floor((totalSeconds % 3600) / 60)
        var seconds = totalSeconds % 60
        var mm = (minutes < 10 ? "0" : "") + minutes
        var ss = (seconds < 10 ? "0" : "") + seconds
        if (hours > 0) return hours + ":" + mm + ":" + ss
        return mm + ":" + ss
    }

    Connections {
        target: root.hasPlayer ? mediaPlayer : null

        function onPositionChanged() {
            if (!positionSlider.pressed && root.hasPlayer) {
                positionSlider.value = mediaPlayer.position
            }
        }
    }
}
