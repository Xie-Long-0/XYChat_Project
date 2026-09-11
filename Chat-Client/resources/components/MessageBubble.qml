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
    property real fileSizeBytes: 0
    // available=本地已就绪、downloading=下载中、missing=需下载
    property string fileState: "missing"
    property real fileProgress: 0
    // M8.3: 图片元数据。fileThumb 为 base64 的 JPEG 缩略图，随清单经 E2EE
    // 到达（不含任何密钥），因此可在下载原图之前直接展示预览
    property int fileWidth: 0
    property int fileHeight: 0
    property string fileThumb: ""

    signal downloadRequested()
    signal saveRequested()

    // M9 特性栈：右键菜单操作（由 ChatView 转发到 MainPage）
    signal editRequested()
    signal deleteRequested()

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
                // 文件消息不展示正文（正文是清单，已置空），改走下方文件面板
                Label {
                    id: contentLabel
                    visible: !isFileMessage || deleted
                    width: Math.min(implicitWidth, messageBubble.maxContentWidth)
                    text: deleted ? "此消息已删除"
                                  : (undecryptable ? "⚠ 无法解密此消息" : content)
                    wrapMode: Text.Wrap
                    font.pixelSize: Theme.fontSizeMedium
                    font.italic: undecryptable || deleted
                    color: deleted ? Theme.textTertiary
                                   : (undecryptable ? Theme.textTertiary : Theme.textPrimary)
                    textFormat: Text.PlainText
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
                            Label {
                                anchors.centerIn: parent
                                text: "📎"
                                color: Theme.textOnPrimary
                                font.pixelSize: Theme.fontSizeMedium
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

                    // 消息状态图标（仅自己的消息）
                    Label {
                        visible: isMine && status !== "" && !deleted
                        text: {
                            switch (status) {
                                case "sending": return "⏳"
                                case "sent": return "✓"
                                case "delivered": return "✓✓"
                                case "read": return "✓✓"
                                case "failed": return "⚠"
                                default: return ""
                            }
                        }
                        font.pixelSize: Theme.fontSizeSmall - 1
                        color: status === "read" ? Theme.primaryColor : Theme.textTertiary
                    }
                }
            }

            // M9: 右键菜单（仅自己的、未删除的消息可编辑/删除）
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                onClicked: function(mouse) {
                    if (mouse.button === Qt.RightButton && isMine && !deleted) {
                        contextMenu.popup()
                    }
                }
            }

            Menu {
                id: contextMenu
                MenuItem {
                    text: "编辑"
                    // M8.2: 文件消息不可编辑正文（服务端也会拒）：编辑只能改写
                    // 正文而 messages.file_id 不变，会使清单里的 fileId/密钥与
                    // 服务端授权失配。正确做法是删除后重发
                    enabled: !isFileMessage
                    onTriggered: messageBubble.editRequested()
                }
                MenuItem {
                    text: "删除"
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
}
