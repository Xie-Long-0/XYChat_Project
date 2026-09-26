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
    // M11B B1: 多选模式（选中态由 ChatView 的选择集持有并驱动，本组件只渲染指示
    // 与上报点击——选择集属视图级状态，气泡 delegate 销毁重建不得丢选择）
    signal selectionToggled()
    signal selectionRequested()
    // M11B B2: 回复（引用）本消息 / 点引用块跳到被引用消息
    signal replyRequested()
    // 参数是被引用消息的 ID——点击引用块要跳到**被引用**那条，不是承载引用的这条
    signal replyJumpRequested(int messageId)

    property string clientMessageId: ""
    property bool selectionMode: false
    property bool selected: false
    // M13: 只读会话（已退出的群聊）——回复/多选/编辑/删除一律收起，仅留复制
    property bool readOnly: false

    // M11B B2: 引用（回复）。摘要由客户端本地渲染（服务端只有密文），
    // replyToSender 是服务端回传的被引用者名——仅在本地预览表未命中时用作回退
    property int replyToMessageId: 0
    property string replyToSender: ""
    property string replyPreviewSender: ""
    property string replyPreviewText: ""
    property bool replyPreviewDeleted: false
    // M11B B2: 跳转引用后的短暂高亮（由 ChatView 定时清除）
    property bool highlighted: false

    // 气泡内容区可用宽度上限
    readonly property int maxContentWidth: Theme.messageMaxWidth - Theme.spacingMedium * 2

    // 文件面板目标宽度（缩略图、进度条、文件名省略宽度均以此为准）
    readonly property int filePanelWidth: Math.min(260, maxContentWidth)

    // 缩略图最大高度：仅作上限约束，显示尺寸按原始宽高比换算
    readonly property int thumbMaxHeight: 260
    readonly property real thumbScale: (fileWidth > 0 && fileHeight > 0)
        ? Math.min(filePanelWidth / fileWidth, thumbMaxHeight / fileHeight) : 0
    readonly property int thumbWidth: thumbScale > 0 ? Math.round(fileWidth * thumbScale) : filePanelWidth
    readonly property int thumbHeight: thumbScale > 0 ? Math.round(fileHeight * thumbScale) : 180

    // 气泡内部内容所需宽度：文本自然宽度（超上限则折行到上限）、文件面板宽度、
    // 时间/状态行宽度、最小宽度四者取最大。
    // 修复：此前漏算文件面板宽度——文件消息正文被置空、元信息行只有时间+状态，
    // 气泡被压到最小宽度（约 60px），而 filePanel 固定 260px，
    // 于是文件名、"另存为"按钮等整体溢出气泡并被窗口裁切。
    readonly property real contentWidth: {
        var w = 60 - Theme.spacingMedium * 2
        if (contentLabel.visible) {
            w = Math.max(w, Math.min(contentLabel.implicitWidth, maxContentWidth))
        }
        if (filePanel.visible) {
            w = Math.max(w, filePanel.width)
        }
        // M11B B2: 引用块也要参与宽度计算，否则短消息（如 "ok"）的引用块
        // 会被压到最小宽度里、摘要被截得只剩几个字
        if (replyQuote.visible) {
            w = Math.max(w, Math.min(replyQuote.implicitWidth, maxContentWidth))
        }
        return Math.max(w, metaRow.width)
    }

    // 悬浮操作按钮是否放在气泡外侧。外侧（自己的消息在左、对方消息在右）不会
    // 遮挡正文；窄窗口下长消息会占满整行、外侧放不下，此时 actionsOutside 为
    // false，退回气泡内部右上角——宁可轻微遮挡，也不能把按钮推到可视区之外
    //（那样等于点不到）。两侧对称：气泡靠窗口边只留 spacingMedium，故外侧
    // 剩余宽度相同，判断只需一条公式。
    // 依赖 actionButtons.width（Positioner 的内容宽度）而非另算一份按钮数量，
    // 避免"编辑按钮仅自己的文本消息可见"这类条件在两处各写一遍而漂移
    readonly property bool actionsOutside:
        (width - Theme.spacingMedium - bubbleColumn.width)
            >= actionButtons.width + Theme.spacingXSmall

    // 悬浮操作按钮的显隐判定区：覆盖整行（气泡 + 外侧按钮 + 两者之间的间隙）。
    // Qt 的 hover 只沿父子链向上传播、不会传给被遮挡的兄弟节点（见
    // qquickdeliveryagent.cpp 的 deliverHoverEventRecursive 注释），因此若
    // 分别监听气泡与各按钮，光标穿过间隙的那一帧两边都不 hover，按钮会闪一下。
    // root 是气泡与按钮的共同祖先，必然收到 hover，用它统一判定最稳。
    // 用 HoverHandler 而非 MouseArea：它不消费鼠标事件，不会抢走气泡的右键菜单
    HoverHandler { id: rowHover }

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
            width: messageBubble.contentWidth + Theme.spacingMedium * 2
            height: contentColumn.implicitHeight + Theme.spacingMedium * 2
            radius: Theme.radiusBubble
            color: isMine ? Theme.bubbleOutColor : Theme.bubbleInColor
            // M11B B2: 跳转引用后短暂高亮（与多选选中态同一视觉口径）
            border.width: (selected || highlighted) ? 2 : 1
            border.color: (selected || highlighted) ? Theme.primaryColor
                                  : (isMine ? Theme.bubbleOutBorderColor : Theme.bubbleInBorderColor)

            Column {
                id: contentColumn
                x: Theme.spacingMedium
                y: Theme.spacingMedium
                width: bubbleRect.width - Theme.spacingMedium * 2
                spacing: Theme.spacingXSmall

                // M11B B2: 引用块。摘要由本地引用预览表渲染（服务端只有密文）；
                // 表未命中（被引用消息不在本页）时退回服务端带出的发送者名 +
                // "原消息不可见"占位。点击跳到被引用消息
                Rectangle {
                    id: replyQuote
                    visible: messageBubble.replyToMessageId > 0 && !deleted
                    width: contentColumn.width
                    height: quoteColumn.implicitHeight + Theme.spacingSmall
                    radius: Theme.radiusSmall
                    color: isMine ? Theme.bubbleOutBorderColor : Theme.hoverColor
                    // 自然宽度参与 contentWidth 计算（见那里的说明）
                    implicitWidth: quoteColumn.implicitWidth + Theme.spacingMedium

                    Row {
                        anchors.fill: parent
                        anchors.margins: Theme.spacingXSmall
                        spacing: Theme.spacingXSmall

                        Rectangle {
                            width: 3
                            height: parent.height
                            radius: 1.5
                            color: Theme.primaryColor
                        }

                        Column {
                            id: quoteColumn
                            spacing: 0

                            Label {
                                text: {
                                    if (messageBubble.replyPreviewSender.length > 0) {
                                        return messageBubble.replyPreviewSender
                                    }
                                    if (messageBubble.replyToSender.length > 0) {
                                        return messageBubble.replyToSender
                                    }
                                    return "引用消息"
                                }
                                width: Math.min(implicitWidth,
                                                messageBubble.maxContentWidth - Theme.spacingMedium)
                                font.pixelSize: Theme.fontSizeSmall - 1
                                font.weight: Font.DemiBold
                                color: Theme.primaryColor
                                elide: Text.ElideRight
                            }

                            Label {
                                text: messageBubble.replyPreviewText
                                width: Math.min(implicitWidth,
                                                messageBubble.maxContentWidth - Theme.spacingMedium)
                                font.pixelSize: Theme.fontSizeSmall - 1
                                font.italic: messageBubble.replyPreviewDeleted
                                color: Theme.textSecondary
                                elide: Text.ElideRight
                                maximumLineCount: 1
                            }
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: messageBubble.replyJumpRequested(messageBubble.replyToMessageId)
                    }
                }

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
                    width: messageBubble.filePanelWidth
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
                        // 按清单里的原始宽高比换算显示尺寸，避免竖图被塞进
                        // 定宽 × 180 高后上下留出大片空白（"气泡大小异常"的一部分）
                        width: messageBubble.thumbWidth
                        height: messageBubble.thumbHeight
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
                        // 统一用 AppButton：裸 Button 会落到 Controls 默认（浅色）样式，
                        // 在暗色主题气泡里是一块刺眼的白底，且宽度不受控
                        AppButton {
                            visible: fileState === "missing"
                            text: "下载"
                            height: 28
                            font.pixelSize: Theme.fontSizeSmall
                            onClicked: downloadRequested()
                        }
                        AppButton {
                            visible: fileState === "available"
                            text: "另存为"
                            height: 28
                            font.pixelSize: Theme.fontSizeSmall
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
                            cursorShape: parent.status === "failed" && !messageBubble.readOnly
                                         ? Qt.PointingHandCursor : Qt.ArrowCursor
                            // M13: 只读会话（已退出群聊）重发必失败，不予响应
                            enabled: parent.status === "failed" && !messageBubble.readOnly
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
                // 多选模式下行级左键由选择覆盖层接管；右键菜单同时让位，
                // 避免"以为在勾选、实际弹出菜单"的错位交互
                enabled: !selectionMode
                onClicked: {
                    // M11B B1: 对方消息也开放右键（复制/多选），编辑/删除两项
                    // 仅对自己的消息可见
                    if (!deleted) contextMenu.popup()
                }
            }

            // 悬浮操作按钮。原先锚在气泡内部右上角，会直接盖住正文（短消息
            // 如 "Hello" 几乎被三个按钮压掉大半），故改为气泡外侧；
            // 窄窗口下退回内部（见 actionsOutside）
            Row {
                id: actionButtons
                spacing: 2
                // 三种落位用显式 x/y 表达，避免 anchors 在条件切换时互相冲突：
                //   外侧且自己的消息 → 气泡左侧；外侧且对方消息 → 气泡右侧；
                //   放不下 → 气泡内部右上角
                x: messageBubble.actionsOutside
                   ? (isMine ? -Theme.spacingXSmall - width
                             : bubbleRect.width + Theme.spacingXSmall)
                   : bubbleRect.width - Theme.spacingXSmall - width
                y: messageBubble.actionsOutside
                   ? Math.round((bubbleRect.height - height) / 2)
                   : Theme.spacingXSmall

                // 显隐由整行 hover 统一驱动（见根节点的 HoverHandler）。
                // 只改 opacity，让淡出动画真正可见；enabled 保证不可见时不吞点击
                //（disabled 会向下传递给按钮上的 MouseArea）。
                // 多选模式下整行让位给选择覆盖层，悬浮按钮一并隐藏
                opacity: (!deleted && !selectionMode && rowHover.hovered) ? 1 : 0
                visible: opacity > 0
                enabled: opacity > 0
                Behavior on opacity { NumberAnimation { duration: Theme.animationFast } }

                // M11B B2: 回复（引用）本条消息。乐观/失败气泡还没有服务端 ID，
                // 引用它们只会得到 messageId=0 的空引用，故不可回复；
                // M13: 只读会话发不出消息，回复入口同样收起
                Rectangle {
                    visible: !messageBubble.readOnly
                    width: 22; height: 22; radius: 11
                    opacity: messageId > 0 ? 1 : 0.4
                    color: replyMouseArea.containsMouse && messageId > 0
                           ? Theme.hoverColor : "transparent"
                    MouseArea {
                        id: replyMouseArea
                        anchors.fill: parent
                        enabled: messageBubble.messageId > 0
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: messageBubble.replyRequested()
                    }
                    Icon {
                        anchors.centerIn: parent
                        name: "reply"
                        size: 11
                        iconColor: Theme.textSecondary
                    }
                }

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
                    visible: isMine && !isFileMessage && !messageBubble.readOnly
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
                    visible: isMine && !messageBubble.readOnly
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

            // M9 特性栈：右键菜单。用 AppMenu/AppMenuItem 而非裸 Menu/MenuItem
            //（Basic 样式在暗色主题下是浅色面板 + 不可见的白图标，见两个组件的说明）
            AppMenu {
                id: contextMenu
                AppMenuItem {
                    text: "回复"
                    iconName: "reply"
                    // 乐观/失败气泡无服务端 ID，引用它们只会得到空引用；
                    // M13: 只读会话发不出消息，回复入口一并收起
                    visible: !messageBubble.readOnly
                    enabled: messageId > 0
                    onTriggered: messageBubble.replyRequested()
                }
                AppMenuItem {
                    text: "复制"
                    iconName: "copy"
                    // 文件消息正文是清单（已置空），无内容可复制
                    enabled: !isFileMessage && content.length > 0
                    onTriggered: messageBubble.copyRequested()
                }
                AppMenuItem {
                    text: "多选"
                    iconName: "checkbox-checked"
                    // 乐观气泡尚无服务端 ID，选择集按 messageId 记，故不可选
                    visible: !messageBubble.readOnly
                    enabled: messageId > 0
                    onTriggered: messageBubble.selectionRequested()
                }
                AppMenuItem {
                    text: "编辑"
                    iconName: "edit"
                    visible: isMine && !messageBubble.readOnly
                    enabled: !isFileMessage
                    onTriggered: messageBubble.editRequested()
                    ToolTip.visible: hovered && !enabled
                    ToolTip.text: "文件消息不可编辑，请删除后重发"
                }
                AppMenuItem {
                    text: "删除"
                    iconName: "delete"
                    visible: isMine && !messageBubble.readOnly
                    danger: true
                    onTriggered: messageBubble.deleteRequested()
                }
            }
        }
    }

    // M11B B1: 选中指示圆点。放在气泡与对齐边之间的空白处（自己的消息在左、
    // 对方在右），与 Telegram 一致：不遮挡正文，也不会把气泡推离窗口边缘
    Rectangle {
        id: selectionIndicator
        visible: messageBubble.selectionMode
        width: 22
        height: 22
        color: "transparent"
        x: isMine ? bubbleColumn.x - Theme.spacingMedium - width
                  : bubbleColumn.x + bubbleColumn.width + Theme.spacingMedium
        y: bubbleColumn.y + bubbleRect.y + Math.round((bubbleRect.height - height) / 2)

        Icon {
            anchors.centerIn: parent
            name: messageBubble.selected ? "checkbox-checked" : "checkbox-unchecked"
            size: 20
            iconColor: messageBubble.selected ? Theme.primaryColor : Theme.textTertiary
        }
    }

    // M11B B1: 多选模式的行级点击区。声明在最后（z 序高于气泡内容与悬浮按钮），
    // 选中模式下接管整行左键：正文 TextEdit 的文本选择、附件面板的下载/预览
    // 按钮在此期间都不应响应——整行即"勾选开关"。disabled 时不消费任何事件
    MouseArea {
        anchors.fill: parent
        enabled: messageBubble.selectionMode
        acceptedButtons: Qt.LeftButton
        cursorShape: Qt.PointingHandCursor
        onClicked: messageBubble.selectionToggled()
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
