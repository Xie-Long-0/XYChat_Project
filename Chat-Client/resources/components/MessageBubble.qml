import QtQuick
import QtQuick.Controls

import "../theme"

Item {
    id: messageBubble
    // 宽度由外层 delegate 指定；高度由内容驱动
    height: bubbleColumn.height + Theme.spacingSmall * 2

    property bool isMine: false
    property string senderName: ""
    property string content: ""
    property string time: ""
    property string status: ""
    // M6: 端到端加密消息无法解密（无对应预密钥/新设备无历史密钥）
    property bool undecryptable: false
    // M9 特性栈：消息编辑/删除状态（仅自己消息可编辑/删除）
    property int messageId: 0
    property bool edited: false
    property bool deleted: false

    // M8.2: 文件消息的脱敏展示字段（由 C++ 侧从已解密的清单里取出，
    // 不包含文件密钥与 nonce；正文 content 对文件消息已被置空）
    property bool isFileMessage: false
    property string fileName: ""
    property string fileMime: ""
    property real fileSizeBytes: 0
    // available=本地已就绪、downloading=下载中、missing=需下载
    property string fileState: "missing"
    property real fileProgress: 0
    // M8.3: 图片元数据。fileThumb 为 base64 的 JPEG 缩略图，随清单经 E2EE
    // 到达（不含任何密钥），因此可在下载原图之前直接展示预览
    property int fileWidth: 0
    property int fileHeight: 0
    property string fileThumb: ""
    // M8.3b: 音视频时长（毫秒，不含密钥）。为 0 表示未知，UI 隐藏时长标签
    property real fileDurationMs: 0

    signal downloadRequested()
    signal saveRequested()
    // M8.3: 点击缩略图/图标请求应用内大图预览（仅当本地已就绪时有意义）
    signal previewRequested()
    // M8.3b: 点击播放按钮请求应用内音视频播放（仅当本地已就绪时有意义）
    signal playRequested()

    // 是否图片类型：只有图片能在应用内解码预览（清单里的 MIME 经 E2EE 到达）
    readonly property bool isImageFile: fileMime.indexOf("image/") === 0
    // M8.3b: 音视频类型判定（清单里的 MIME 经 E2EE 到达，服务端不可见）
    readonly property bool isAudioFile: fileMime.indexOf("audio/") === 0
    readonly property bool isVideoFile: fileMime.indexOf("video/") === 0

    signal editRequested()
    signal deleteRequested()
    signal resendRequested()
    signal copyRequested()

    property string clientMessageId: ""

    // 气泡内容区可用宽度上限
    readonly property int maxContentWidth: Theme.messageMaxWidth - Theme.spacingMedium * 2

    Column {
        id: bubbleColumn
        // 自己的消息靠右，对方的消息靠左
        anchors.right: isMine ? parent.right : undefined
        anchors.rightMargin: isMine ? Theme.spacingMedium : 0
        anchors.left: isMine ? undefined : parent.left
        anchors.leftMargin: isMine ? 0 : Theme.spacingMedium
        spacing: 2

        // 发送者名称（仅对方消息显示）
        Label {
            text: senderName
            font.pixelSize: Theme.fontSizeSmall
            font.weight: Font.DemiBold
            color: Theme.primaryColor
            visible: !isMine && senderName !== ""
        }

        // 气泡主体：宽度随内容自适应，超过上限自动换行
        Rectangle {
            id: bubbleRect
            width: Math.max(
                       Math.min(contentLabel.implicitWidth, messageBubble.maxContentWidth),
                       metaRow.width,
                       60 - Theme.spacingMedium * 2) + Theme.spacingMedium * 2
            height: contentColumn.implicitHeight + Theme.spacingMedium * 2
            radius: Theme.radiusBubble
            color: isMine ? Theme.bubbleOutColor : Theme.bubbleInColor
            border.width: 1
            border.color: isMine ? Theme.bubbleOutBorderColor : Theme.bubbleInBorderColor

            Column {
                id: contentColumn
                x: Theme.spacingMedium
                y: Theme.spacingMedium
                width: bubbleRect.width - Theme.spacingMedium * 2
                spacing: Theme.spacingXSmall

                // 消息内容：短消息单行自然宽度，长消息在最大宽度内自动换行。
                // P2.2: 用只读 TextEdit 替代 Label，让正文可鼠标选中并 Ctrl+C
                // 复制，不显示闪烁光标；文件消息不展示正文（正文是清单，已置空）
                TextEdit {
                    id: contentLabel
                    visible: !isFileMessage || deleted
                    width: Math.min(implicitWidth, messageBubble.maxContentWidth)
                    text: deleted ? "此消息已删除"
                                  : (undecryptable ? "无法解密此消息" : content)
                    wrapMode: TextEdit.Wrap
                    font.pixelSize: Theme.fontSizeMedium
                    font.italic: undecryptable || deleted
                    color: deleted ? Theme.textTertiary
                                   : (undecryptable ? Theme.textTertiary : Theme.textPrimary)
                    textFormat: TextEdit.PlainText
                    readOnly: true
                    selectByMouse: true
                    selectionColor: Theme.primaryColor
                    selectedTextColor: Theme.textOnPrimary
                    activeFocusOnPress: true
                    cursorVisible: false
                }

                // M8.2: 文件消息面板（图标 + 文件名 + 大小 + 下载/保存）
                Column {
                    id: filePanel
                    visible: isFileMessage && !deleted
                    width: Math.min(260, messageBubble.maxContentWidth)
                    spacing: Theme.spacingSmall

                    // M8.3: 内联缩略图。仅在解码成功后显示（status === Ready），
                    // 因此清单被截断或图像损坏时不会留下空白区域，
                    // 下方的图标行依旧能完整展示文件名与大小
                    Image {
                        id: thumbImage
                        source: fileThumb.length > 0
                                ? "data:image/jpeg;base64," + fileThumb
                                : ""
                        visible: fileThumb.length > 0 && status === Image.Ready
                        fillMode: Image.PreserveAspectFit
                        width: filePanel.width
                        height: 180
                        asynchronous: true
                        smooth: true
                        cache: true

                        // M8.3: 图片点击看大图；M8.3b: 视频点击播放。
                        // 图像源 image://xyfile/<messageId> 由 C++ 从密文缓存
                        // 逐片解密并在内存中解码（明文不落盘），仅在本地已就绪时可点
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            enabled: (isImageFile || isVideoFile) && fileState === "available"
                            onClicked: isVideoFile ? messageBubble.playRequested()
                                                   : messageBubble.previewRequested()
                        }

                        // M8.3b: 视频封面中央叠加播放按钮（半透明圆形 + ▶）
                        Rectangle {
                            anchors.centerIn: parent
                            visible: isVideoFile && fileState === "available"
                            width: 48; height: 48; radius: 24
                            color: "#80000000"
                            Icon {
                                anchors.centerIn: parent
                                name: "play"
                                size: 20
                                iconColor: "white"
                            }
                        }

                        // M8.3b: 右下角时长标签（音视频，半透明背景）
                        Rectangle {
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            anchors.margins: 6
                            visible: fileDurationMs > 0
                            color: "#80000000"
                            radius: Theme.radiusSmall
                            width: thumbDurationLabel.implicitWidth + 12
                            height: thumbDurationLabel.implicitHeight + 6
                            Label {
                                id: thumbDurationLabel
                                anchors.centerIn: parent
                                text: messageBubble.formatDuration(fileDurationMs)
                                color: "white"
                                font.pixelSize: Theme.fontSizeSmall - 1
                            }
                        }
                    }

                    // M8.3: 图片像素尺寸（仅在清单带了尺寸时展示）
                    Label {
                        visible: fileWidth > 0 && fileHeight > 0
                        text: fileWidth + " × " + fileHeight
                        font.pixelSize: Theme.fontSizeSmall - 1
                        color: Theme.textTertiary
                    }

                    Row {
                        spacing: Theme.spacingSmall

                        Rectangle {
                            width: 34; height: 34
                            radius: Theme.radiusSmall
                            color: Theme.primaryColor
                            Icon {
                                anchors.centerIn: parent
                                name: (isAudioFile || isVideoFile) ? "play" : "attach"
                                size: 16
                                iconColor: Theme.textOnPrimary
                            }
                            // M8.3: 无内联缩略图的图片点击预览；M8.3b: 音视频点击播放
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                enabled: fileState === "available"
                                         && (isImageFile || isAudioFile || isVideoFile)
                                         && thumbImage.status !== Image.Ready
                                onClicked: (isAudioFile || isVideoFile)
                                           ? messageBubble.playRequested()
                                           : messageBubble.previewRequested()
                            }
                        }

                        Column {
                            spacing: 2
                            Label {
                                text: fileName
                                width: Math.min(implicitWidth, filePanel.width - 46)
                                elide: Text.ElideMiddle
                                font.pixelSize: Theme.fontSizeMedium
                                color: Theme.textPrimary
                            }
                            Label {
                                text: messageBubble.formatSize(fileSizeBytes)
                                font.pixelSize: Theme.fontSizeSmall - 1
                                color: Theme.textTertiary
                            }
                            // M8.3b: 音视频时长（无封面时在文件名下方展示，
                            // 有封面时已在缩略图右下角叠加，此处隐藏避免重复）
                            Label {
                                visible: fileDurationMs > 0 && thumbImage.status !== Image.Ready
                                text: messageBubble.formatDuration(fileDurationMs)
                                font.pixelSize: Theme.fontSizeSmall - 1
                                color: Theme.textTertiary
                            }
                        }
                    }

                    // 下载中：进度条（值由引擎的 taskProgress 折算为 0..1）
                    ProgressBar {
                        visible: fileState === "downloading"
                        width: filePanel.width
                        from: 0; to: 1
                        value: fileProgress
                    }

                    Row {
                        spacing: Theme.spacingSmall
                        Button {
                            visible: fileState === "missing"
                            text: "下载"
                            onClicked: downloadRequested()
                        }
                        Button {
                            visible: fileState === "available"
                            text: "另存为"
                            onClicked: saveRequested()
                        }
                        Label {
                            visible: fileState === "downloading"
                            text: "下载中…"
                            font.pixelSize: Theme.fontSizeSmall - 1
                            color: Theme.textTertiary
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }
                }

                // 时间和状态
                Row {
                    id: metaRow
                    spacing: Theme.spacingXSmall
                    // 右对齐：Row 不支持对齐，通过 x 偏移实现
                    x: contentColumn.width - width

                    Label {
                        text: time
                        font.pixelSize: Theme.fontSizeSmall - 1
                        color: Theme.textTertiary
                    }

                    // M9: “已编辑”标记（删除后不展示）
                    Label {
                        visible: edited && !deleted
                        text: "已编辑"
                        font.pixelSize: Theme.fontSizeSmall - 1
                        font.italic: true
                        color: Theme.textTertiary
                    }

                    Icon {
                        visible: isMine && status !== "" && !deleted
                        name: {
                            switch (status) {
                                case "sending": return "clock"
                                case "sent": return "check"
                                case "delivered": return "check-double"
                                case "read": return "check-double"
                                case "failed": return "warning"
                                default: return ""
                            }
                        }
                        size: 12
                        iconColor: {
                            if (status === "read") return Theme.primaryColor
                            if (status === "failed") return Theme.errorColor
                            return Theme.textTertiary
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: parent.status === "failed" ? Qt.PointingHandCursor : Qt.ArrowCursor
                            enabled: parent.status === "failed"
                            onClicked: messageBubble.resendRequested()
                        }
                    }
                }
            }

            MouseArea {
                id: bubbleMouse
                anchors.fill: parent
                hoverEnabled: true
                acceptedButtons: Qt.RightButton
                onClicked: {
                    if (isMine && !deleted) contextMenu.popup()
                }
            }

            Row {
                id: actionButtons
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: Theme.spacingXSmall
                spacing: 2
                // P2.2: 悬浮显示。同时监听气泡与各操作按钮的 hover，避免光标
                // 从气泡移到按钮时因兄弟节点 hover 不传递而闪烁消失
                visible: !deleted && (bubbleMouse.containsMouse
                                      || copyMouseArea.containsMouse
                                      || editMouseArea.containsMouse
                                      || deleteMouseArea.containsMouse)
                opacity: visible ? 1 : 0
                Behavior on opacity { NumberAnimation { duration: Theme.animationFast } }

                Rectangle {
                    width: 22; height: 22; radius: 11
                    color: copyMouseArea.containsMouse ? Theme.hoverColor : "transparent"
                    MouseArea {
                        id: copyMouseArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: messageBubble.copyRequested()
                    }
                    Icon {
                        anchors.centerIn: parent
                        name: "copy"
                        size: 11
                        iconColor: Theme.textSecondary
                    }
                }

                Rectangle {
                    visible: isMine && !isFileMessage
                    width: 22; height: 22; radius: 11
                    color: editMouseArea.containsMouse ? Theme.hoverColor : "transparent"
                    MouseArea {
                        id: editMouseArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: messageBubble.editRequested()
                    }
                    Icon {
                        anchors.centerIn: parent
                        name: "edit"
                        size: 11
                        iconColor: Theme.textSecondary
                    }
                }

                Rectangle {
                    visible: isMine
                    width: 22; height: 22; radius: 11
                    color: deleteMouseArea.containsMouse ? Theme.hoverColor : "transparent"
                    MouseArea {
                        id: deleteMouseArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: messageBubble.deleteRequested()
                    }
                    Icon {
                        anchors.centerIn: parent
                        name: "delete"
                        size: 11
                        iconColor: Theme.textSecondary
                    }
                }
            }

            Menu {
                id: contextMenu
                MenuItem {
                    text: "复制"
                    icon.source: "qrc:/icons/copy.svg"
                    icon.width: 14; icon.height: 14
                    icon.color: Theme.textPrimary
                    onTriggered: messageBubble.copyRequested()
                }
                MenuItem {
                    text: "编辑"
                    icon.source: "qrc:/icons/edit.svg"
                    icon.width: 14; icon.height: 14
                    icon.color: Theme.textPrimary
                    enabled: !isFileMessage
                    onTriggered: messageBubble.editRequested()
                    ToolTip.visible: hovered && !enabled
                    ToolTip.text: "文件消息不可编辑，请删除后重发"
                }
                MenuItem {
                    text: "删除"
                    icon.source: "qrc:/icons/delete.svg"
                    icon.width: 14; icon.height: 14
                    icon.color: Theme.errorColor
                    onTriggered: messageBubble.deleteRequested()
                }
            }
        }
    }

    // M8.2: 字节数转可读大小（清单里的 plainSize 为加密前字节数）
    function formatSize(bytes) {
        if (bytes <= 0) {
            return ""
        }
        if (bytes < 1024) {
            return bytes + " B"
        }
        if (bytes < 1024 * 1024) {
            return (bytes / 1024).toFixed(1) + " KB"
        }
        if (bytes < 1024 * 1024 * 1024) {
            return (bytes / (1024 * 1024)).toFixed(1) + " MB"
        }
        return (bytes / (1024 * 1024 * 1024)).toFixed(2) + " GB"
    }

    // M8.3b: 毫秒转可读时长（mm:ss 或 h:mm:ss）。为 0 返回空串（UI 隐藏标签）
    function formatDuration(ms) {
        if (ms <= 0) {
            return ""
        }
        var totalSeconds = Math.floor(ms / 1000)
        var hours = Math.floor(totalSeconds / 3600)
        var minutes = Math.floor((totalSeconds % 3600) / 60)
        var seconds = totalSeconds % 60
        var mm = (minutes < 10 ? "0" : "") + minutes
        var ss = (seconds < 10 ? "0" : "") + seconds
        if (hours > 0) {
            return hours + ":" + mm + ":" + ss
        }
        return mm + ":" + ss
    }
}
