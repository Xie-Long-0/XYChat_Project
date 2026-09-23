import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

import "../theme"

Rectangle {
    id: messageInput
    height: inputColumn.implicitHeight + Theme.spacingMedium * 2
    color: Theme.inputBackground

    signal messageSent(string text)
    signal attachmentSelected(var fileUrl)
    // P3.2: 用户正在输入（文本变化且非空时触发，供上层发“正在输入”信号）
    signal typingActivity()
    // M11B B2: 取消回复（输入区引用条的关闭按钮）
    signal replyCancelRequested()

    property int maxLength: 4096
    // M11A A4: 暴露输入框文本供外部读写（草稿保存/恢复）
    property alias text: inputField.text
    // M11B B2: 正在回复的消息（{messageId, senderName, text, isMine}）；null 表示无引用。
    // 由 ChatView 持有状态（引用属于会话视图维度，不随输入框重建丢失）
    property var replyInfo: null
    // M11B B4: 最近使用的 emoji（最多 16 个，经 AppSettings 持久化）
    property var recentEmojis: []
    // M11B B4: 常用 emoji 面板（8 列固定网格，无需滚动）
    readonly property var emojiPalette: [
        "😀", "😄", "😁", "😂", "🤣", "😊", "😍", "😘",
        "😎", "🤔", "😅", "😭", "😡", "🥺", "😴", "🤗",
        "👍", "👎", "👌", "🙏", "👏", "💪", "🤝", "✌️",
        "❤️", "💔", "🎉", "🔥", "✨", "⭐", "🌟", "💯",
        "🍺", "☕", "🍕", "🎂", "🎁", "📌", "📎", "✅",
        "❌", "❓", "❗", "⚠️", "🚀", "🐱", "🐶", "🍀"
    ]
    readonly property int emojiMaxRecent: 16
    // M11B B4: 面板网格几何。列数与单元尺寸定了，面板宽度就必须是
    // 列数 × 单元宽 + 内边距（此前面板宽 328 而 8 列 × 41 = 328 未扣内边距，
    // 最后一列被面板右边缘压住）
    readonly property int emojiColumns: 8
    readonly property int emojiCellSize: 40
    readonly property int emojiCellHeight: 34

    Component.onCompleted: {
        // M11B B4: 从设置恢复最近使用（空格分隔；emoji 序列本身不含空格）
        if (typeof appSettings !== "undefined" && appSettings.recentEmojis.length > 0) {
            recentEmojis = appSettings.recentEmojis.split(" ")
        }
    }

    FileDialog {
        id: attachmentDialog
        title: qsTr("选择要发送的文件")
        fileMode: FileDialog.OpenFile
        onAccepted: {
            if (selectedFile.toString().length > 0) {
                attachmentSelected(selectedFile)
            }
        }
    }

    Rectangle {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 1
        color: Theme.separatorColor
    }

    ColumnLayout {
        id: inputColumn
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: Theme.spacingLarge
        anchors.rightMargin: Theme.spacingLarge
        spacing: Theme.spacingXSmall

        // M11B B2: 引用条（回复某人时显示在输入框上方）
        RowLayout {
            id: replyBar
            Layout.fillWidth: true
            visible: messageInput.replyInfo !== null
            spacing: Theme.spacingSmall

            Rectangle {
                Layout.preferredWidth: 3
                Layout.fillHeight: true
                color: Theme.primaryColor
                radius: 1.5
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0

                Label {
                    Layout.fillWidth: true
                    text: {
                        var info = messageInput.replyInfo
                        if (info === null) return ""
                        if (info.isMine) return "回复自己"
                        return info.senderName.length > 0
                               ? "回复 " + info.senderName : "回复消息"
                    }
                    font.pixelSize: Theme.fontSizeSmall
                    font.weight: Font.DemiBold
                    color: Theme.primaryColor
                    elide: Text.ElideRight
                }

                Label {
                    Layout.fillWidth: true
                    text: messageInput.replyInfo !== null ? messageInput.replyInfo.text : ""
                    font.pixelSize: Theme.fontSizeSmall
                    color: Theme.textSecondary
                    elide: Text.ElideRight
                    maximumLineCount: 1
                }
            }

            Rectangle {
                id: replyCloseBtn
                Layout.preferredWidth: 22
                Layout.preferredHeight: 22
                radius: 11
                color: replyCloseMouse.containsMouse ? Theme.hoverColor : "transparent"

                MouseArea {
                    id: replyCloseMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: messageInput.replyCancelRequested()
                }

                Icon {
                    anchors.centerIn: parent
                    name: "close"
                    size: 11
                    iconColor: Theme.textSecondary
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingSmall

            Rectangle {
                id: attachButton
                width: Theme.inputHeight
                height: Theme.inputHeight
                radius: Theme.inputHeight / 2
                color: attachMouse.pressed ? Theme.inputBorderColor : "transparent"
                opacity: typeof fileTransfer !== "undefined" && fileTransfer.enabled ? 1.0 : 0.4

                Behavior on color { ColorAnimation { duration: Theme.animationFast } }

                MouseArea {
                    id: attachMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        if (typeof fileTransfer !== "undefined" && fileTransfer.enabled) {
                            attachmentDialog.open()
                        }
                    }
                }

                Icon {
                    anchors.centerIn: parent
                    name: "attach"
                    size: 18
                    iconColor: Theme.textSecondary
                }
            }

            TextArea {
                id: inputField
                Layout.fillWidth: true
                Layout.minimumHeight: Theme.inputHeight
                Layout.maximumHeight: Theme.inputHeight * 5
                placeholderText: qsTr("输入消息...")
                font.pixelSize: Theme.fontSizeBody
                wrapMode: TextEdit.Wrap
                leftPadding: Theme.spacingMedium
                rightPadding: Theme.spacingMedium
                topPadding: Theme.spacingSmall
                bottomPadding: Theme.spacingSmall
                placeholderTextColor: Theme.inputPlaceholderColor
                color: Theme.textPrimary
                selectByMouse: true
                clip: true

                background: Rectangle {
                    radius: Theme.radiusMedium
                    color: Theme.chatBackground
                    border.width: inputField.activeFocus ? 2 : 1
                    border.color: inputField.activeFocus ? Theme.inputFocusBorderColor : Theme.inputBorderColor

                    Behavior on border.color { ColorAnimation { duration: Theme.animationFast } }
                }

                // Qt 6.8+：信号处理器的隐式参数注入已废弃，必须用带形参的箭头函数显式接收 event
                Keys.onReturnPressed: (event) => {
                    if (event.modifiers & Qt.ShiftModifier) {
                        inputField.insert(inputField.cursorPosition, "\n")
                    } else {
                        sendMessage()
                    }
                    event.accepted = true
                }

                Keys.onEnterPressed: (event) => {
                    if (event.modifiers & Qt.ShiftModifier) {
                        inputField.insert(inputField.cursorPosition, "\n")
                    } else {
                        sendMessage()
                    }
                    event.accepted = true
                }

                // P3.2: 键入时上报“正在输入”（C++ 侧节流，无需在此限频）
                onTextChanged: {
                    if (inputField.text.length > 0) {
                        messageInput.typingActivity()
                    }
                }
            }

            Rectangle {
                id: emojiButton
                Layout.preferredWidth: Theme.inputHeight
                Layout.preferredHeight: Theme.inputHeight
                radius: Theme.inputHeight / 2
                color: emojiMouse.pressed ? Theme.inputBorderColor
                     : (emojiPopup.visible ? Theme.hoverColor : "transparent")

                MouseArea {
                    id: emojiMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: emojiPopup.visible ? emojiPopup.close() : emojiPopup.open()
                }

                Icon {
                    anchors.centerIn: parent
                    name: "emoji"
                    size: 18
                    iconColor: Theme.textSecondary
                }
            }

            Rectangle {
                id: sendButton
                width: Theme.inputHeight
                height: Theme.inputHeight
                radius: Theme.inputHeight / 2
                color: sendMouse.pressed ? Theme.loginButtonPressed
                     : (sendMouse.containsMouse ? Theme.loginButtonHover : Theme.primaryColor)

                Behavior on color { ColorAnimation { duration: Theme.animationFast } }

                MouseArea {
                    id: sendMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: sendMessage()
                }

                Icon {
                    anchors.centerIn: parent
                    name: "send"
                    size: 18
                    iconColor: Theme.textOnPrimary
                }
            }
        }

        Label {
            Layout.alignment: Qt.AlignRight
            text: inputField.text.length + " / " + messageInput.maxLength
            font.pixelSize: Theme.fontSizeCaption
            color: inputField.text.length > messageInput.maxLength
                   ? Theme.errorColor : Theme.textTertiary
            visible: inputField.text.length > 0
        }
    }

    // M11B B4: emoji 面板。用 Popup 而非内联面板：点击外部/按 Esc 自动关闭，
    // 不必自己维护"点别处收起"的全局监听
    Popup {
        id: emojiPopup
        // 供 UI 测试经对象树定位该面板（与主窗口 objectName 同一用法）
        objectName: "emojiPopup"
        // 位置以**窗口坐标**表达并挂到 Overlay 上：Popup 的 x/y 默认相对其
        // parent，而本组件嵌在 ChatView → chatStack 的多层 Item 里，按组件
        // 局部坐标设置会连同 Popup 自身的入界夹取一起把面板放到别处
        parent: Overlay.overlay
        // 尺寸显式给出（不依赖打开时刻的 contentItem 隐式高度）：内容里的
        // GridView 要等布局抛光才有真实高度，Popup 在打开瞬间定下尺寸后
        // 不会再跟随，表现为面板底部裁掉一行
        width: messageInput.emojiColumns * messageInput.emojiCellSize
               + padding * 2
        height: contentItem.implicitHeight + topPadding + bottomPadding
        // 位置在 aboutToShow 里按当时的真实几何显式算（不用 x/y 绑定）：
        // 绑定的首次求值可能早于窗口/版式就绪——那时 mapToItem 得到 0，
        // 之后依赖不再变化，面板就永久停在兜底位置上
        onAboutToShow: messageInput.positionEmojiPanel()
        // 打开瞬间的内容高度可能还没抛光完（最近使用行、网格布局），真实高度在
        // 之后才定型；高度一变就重算位置，否则面板按旧高度落位、底部溢出窗口
        onHeightChanged: if (emojiPopup.visible) messageInput.positionEmojiPanel()
        onOpened: messageInput.positionEmojiPanel()
        padding: Theme.spacingSmall
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        // 面板本身不走系统调色板（Basic 样式下与 Theme 无关），逐项取 Theme
        background: Rectangle {
            radius: Theme.radiusMedium
            color: Theme.windowBackground
            border.width: 1
            border.color: Theme.borderColor
        }

        contentItem: ColumnLayout {
            spacing: Theme.spacingXSmall

            Label {
                Layout.fillWidth: true
                visible: messageInput.recentEmojis.length > 0
                text: "最近使用"
                font.pixelSize: Theme.fontSizeCaption
                color: Theme.textTertiary
            }

            GridView {
                id: recentGrid
                Layout.fillWidth: true
                // 最近使用最多两行（16 个 / 8 列）
                Layout.preferredHeight: messageInput.recentEmojis.length > 0
                                        ? messageInput.emojiCellHeight * 2 : 0
                visible: messageInput.recentEmojis.length > 0
                cellWidth: messageInput.emojiCellSize
                cellHeight: messageInput.emojiCellHeight
                interactive: false
                model: messageInput.recentEmojis

                delegate: Rectangle {
                    width: recentGrid.cellWidth
                    height: recentGrid.cellHeight
                    color: recentMouse.containsMouse ? Theme.hoverColor : "transparent"
                    radius: Theme.radiusSmall

                    Text {
                        anchors.centerIn: parent
                        text: modelData
                        font.pixelSize: 20
                    }

                    MouseArea {
                        id: recentMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: messageInput.insertEmoji(modelData)
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                text: "常用"
                font.pixelSize: Theme.fontSizeCaption
                color: Theme.textTertiary
            }

            GridView {
                id: paletteGrid
                Layout.fillWidth: true
                Layout.preferredHeight: Math.ceil(messageInput.emojiPalette.length
                                                   / messageInput.emojiColumns)
                                        * messageInput.emojiCellHeight
                cellWidth: messageInput.emojiCellSize
                cellHeight: messageInput.emojiCellHeight
                // 正常尺寸下面板完整展示全部行（不滚动）；仅在窗口过矮、
                // 网格被挤压时允许滚动，保证内容仍可达
                interactive: contentHeight > height + 1
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                model: messageInput.emojiPalette

                delegate: Rectangle {
                    width: paletteGrid.cellWidth
                    height: paletteGrid.cellHeight
                    color: paletteMouse.containsMouse ? Theme.hoverColor : "transparent"
                    radius: Theme.radiusSmall

                    Text {
                        anchors.centerIn: parent
                        text: modelData
                        font.pixelSize: 20
                    }

                    MouseArea {
                        id: paletteMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: messageInput.insertEmoji(modelData)
                    }
                }
            }
        }
    }

    // M11B B4: 定位 emoji 面板——挂 Overlay（窗口坐标系），右缘对齐 emoji 按钮、
    // 默认开在按钮上方；上方放不下（窗口过矮）时退回按钮下方。夹取到窗口内，
    // 避免窄窗口下面板被推到可视区之外
    function positionEmojiPanel() {
        var pos = emojiButton.mapToItem(null, 0, 0)
        var winW = emojiPopup.parent ? emojiPopup.parent.width : 0
        var winH = emojiPopup.parent ? emojiPopup.parent.height : 0
        if (Window.window) {
            if (winW <= 0) winW = Window.window.width
            if (winH <= 0) winH = Window.window.height
        }
        var maxX = Math.max(Theme.spacingSmall,
                            winW - emojiPopup.width - Theme.spacingSmall)
        emojiPopup.x = Math.min(Math.max(Theme.spacingSmall,
                                         pos.x + emojiButton.width - emojiPopup.width),
                                maxX)
        var above = pos.y - emojiPopup.height - Theme.spacingXSmall
        var y = above >= Theme.spacingSmall ? above
                                            : pos.y + emojiButton.height + Theme.spacingXSmall
        // 兜底夹取：无论走哪个分支都不让面板越出窗口（矮窗口下宁可盖住输入框，
        // 也不能把内容顶到窗口外——面板本身不滚动）
        emojiPopup.y = Math.min(Math.max(Theme.spacingSmall, y),
                                Math.max(Theme.spacingSmall, winH - emojiPopup.height
                                                                    - Theme.spacingSmall))
    }

    // M11B B4: 插入表情到光标处并记录最近使用（去重前置、上限 16、
    // 经 AppSettings 持久化）。插入后焦点回到输入框，便于连续输入
    function insertEmoji(emoji) {
        inputField.insert(inputField.cursorPosition, emoji)
        inputField.forceActiveFocus()
        var list = recentEmojis.slice()
        var existing = list.indexOf(emoji)
        if (existing >= 0) {
            list.splice(existing, 1)
        }
        list.unshift(emoji)
        if (list.length > emojiMaxRecent) {
            list.length = emojiMaxRecent
        }
        recentEmojis = list
        if (typeof appSettings !== "undefined") {
            appSettings.recentEmojis = list.join(" ")
        }
    }

    function sendMessage() {
        var text = inputField.text.trim()
        if (text.length > 0 && text.length <= messageInput.maxLength) {
            emojiPopup.close()
            messageSent(text)
            inputField.text = ""
        }
    }

    function clearInput() {
        inputField.text = ""
    }

    function setFocus() {
        inputField.forceActiveFocus()
    }
}
