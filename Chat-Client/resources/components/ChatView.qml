import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "../theme"

Rectangle {
    id: chatView
    color: Theme.chatBackground

    property string peerUsername: ""
    property string chatTitle: ""
    property bool hasConversation: false
    // M4.5: 当前登录用户 ID，用于判断消息归属
    property int myUserId: 0
    // M7a: 群会话状态（E2EE 状态提示、成员数副标题、群信息入口）
    property bool isGroup: false
    property int groupMemberCount: 0

    signal sendMessage(string content)
    signal backClicked()
    signal groupInfoRequested()
    // M9 特性栈：消息右键菜单操作（转发到 MainPage）
    signal editRequested(int messageId, string content)
    signal deleteRequested(int messageId)
    // M8.2: 文件消息的下载/另存请求（由 MainPage 接到传输引擎）
    signal fileDownloadRequested(int messageId)
    signal fileSaveRequested(int messageId)
    // M8.2: 附件选择上转（由 MainPage 按会话类型分流到群聊/私聊，与 sendMessage 一致）
    signal attachmentSelected(string filePath)

    // 顶部标题栏
    Rectangle {
        id: chatHeader
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 52
        color: Theme.windowBackground

        Rectangle {
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            height: 1
            color: Theme.separatorColor
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Theme.spacingMedium
            anchors.rightMargin: Theme.spacingMedium
            spacing: Theme.spacingSmall

            // 返回按钮（窄屏时使用）
            Rectangle {
                id: backBtn
                width: 36; height: 36
                radius: 18
                visible: chatView.width < 600
                color: backMouse.containsMouse ? Theme.hoverColor : "transparent"

                MouseArea {
                    id: backMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: chatView.backClicked()
                }

                Canvas {
                    anchors.centerIn: parent
                    width: 12; height: 12
                    onPaint: {
                        var ctx = getContext("2d")
                        ctx.clearRect(0, 0, width, height)
                        ctx.strokeStyle = Theme.textSecondary
                        ctx.lineWidth = 2
                        ctx.beginPath()
                        ctx.moveTo(width, 0)
                        ctx.lineTo(0, height / 2)
                        ctx.lineTo(width, height)
                        ctx.stroke()
                    }
                }
            }

            // 头像
            Rectangle {
                width: Theme.avatarSizeSmall
                height: Theme.avatarSizeSmall
                radius: Theme.avatarSizeSmall / 2
                color: Theme.primaryColor
                visible: hasConversation

                Label {
                    anchors.centerIn: parent
                    text: peerUsername.length > 0 ? peerUsername[0].toUpperCase() : "?"
                    font.pixelSize: Theme.fontSizeMedium
                    font.weight: Font.Bold
                    color: Theme.textOnPrimary
                }
            }

            // 标题
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                visible: hasConversation

                Label {
                    text: chatTitle
                    font.pixelSize: Theme.fontSizeLarge
                    font.weight: Font.DemiBold
                    color: Theme.textPrimary
                    elide: Text.ElideRight
                }

                // M7a: 群会话副标题（成员数）
                Label {
                    visible: isGroup
                    text: groupMemberCount + " 位成员"
                    font.pixelSize: Theme.fontSizeSmall - 1
                    color: Theme.textTertiary
                }
            }

            // M7a: 群信息按钮（打开群成员/管理对话框）
            Rectangle {
                id: groupInfoBtn
                width: 36; height: 36
                radius: 18
                visible: isGroup
                color: groupInfoMouse.containsMouse ? Theme.hoverColor : "transparent"

                MouseArea {
                    id: groupInfoMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: chatView.groupInfoRequested()
                }

                // 多人图标（两个圆 + 肩部弧线）
                Canvas {
                    anchors.centerIn: parent
                    width: 16; height: 16
                    onPaint: {
                        var ctx = getContext("2d")
                        ctx.clearRect(0, 0, width, height)
                        ctx.strokeStyle = Theme.textSecondary
                        ctx.lineWidth = 1.4
                        ctx.beginPath()
                        ctx.arc(6, 5, 2.6, 0, Math.PI * 2)
                        ctx.stroke()
                        ctx.beginPath()
                        ctx.arc(11, 6, 2.1, 0, Math.PI * 2)
                        ctx.stroke()
                        ctx.beginPath()
                        ctx.moveTo(1.5, 13.5)
                        ctx.quadraticCurveTo(6, 8.5, 10.5, 13.5)
                        ctx.stroke()
                    }
                }
            }

            Item {
                Layout.fillWidth: !hasConversation
            }
        }
    }

    // M7b: 群聊端到端加密状态提示（M7a 阶段为“暂未端到端加密”，Sender Keys
    // 落地后改为加密状态）；高度随可见性折叠，避免私聊下锚点链残留空隙
    Rectangle {
        id: groupBanner
        anchors.top: chatHeader.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        height: isGroup ? 28 : 0
        visible: isGroup
        clip: true
        color: Theme.dateDividerColor

        Label {
            anchors.centerIn: parent
            text: "群聊消息已启用端到端加密（Sender Keys），服务端仅存储密文"
            font.pixelSize: Theme.fontSizeSmall - 1
            color: Theme.dateDividerTextColor
        }
    }

    // 消息列表（直接用 ListView 作为滚动容器，ScrollView 不暴露 contentY）
    ListView {
        id: messageListView
        anchors.top: groupBanner.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: messageInput.top
        anchors.topMargin: Theme.spacingSmall
        anchors.bottomMargin: Theme.spacingSmall
        clip: true
        spacing: Theme.spacingXSmall
        verticalLayoutDirection: ListView.TopToBottom
        model: ListModel { id: msgModel }

        ScrollBar.vertical: ScrollBar {
            policy: ScrollBar.AsNeeded
        }

        // M4.5: delegate 内直接条件实例化（不用 Loader，避免 model 角色
        // 在 Loader 加载组件内绑定失效导致气泡空白）
        delegate: Column {
            width: messageListView.width

            // 日期分隔线
            Item {
                width: parent.width
                height: 32
                visible: model.isDivider

                Rectangle {
                    anchors.centerIn: parent
                    height: 22
                    width: dividerLabel.implicitWidth + Theme.spacingLarge * 2
                    radius: 11
                    color: Theme.dateDividerColor

                    Label {
                        id: dividerLabel
                        anchors.centerIn: parent
                        text: model.dividerText
                        font.pixelSize: Theme.fontSizeSmall
                        font.weight: Font.DemiBold
                        color: Theme.dateDividerTextColor
                    }
                }
            }

            // M7a: 群系统消息（居中胶囊展示，不用气泡）
            Item {
                width: parent.width
                height: 28
                visible: !model.isDivider && model.contentType === "system"

                Rectangle {
                    anchors.centerIn: parent
                    height: 22
                    width: systemLabel.implicitWidth + Theme.spacingLarge * 2
                    radius: 11
                    color: Theme.dateDividerColor

                    Label {
                        id: systemLabel
                        anchors.centerIn: parent
                        text: chatView.systemMessageText(model.content)
                        font.pixelSize: Theme.fontSizeSmall
                        color: Theme.dateDividerTextColor
                    }
                }
            }

            // 消息气泡（系统消息不渲染气泡）
            MessageBubble {
                visible: !model.isDivider && model.contentType !== "system"
                width: parent.width
                isMine: model.isMine
                senderName: model.senderUsername
                content: model.content
                time: model.displayTime
                status: model.status || ""
                undecryptable: model.undecryptable === true
                // M9 特性栈：编辑/删除状态与右键菜单
                messageId: model.messageId
                edited: model.edited === true
                deleted: model.deleted === true
                // M8.2: 文件消息的脱敏展示与下载/保存交互
                isFileMessage: model.isFileMessage === true
                fileName: model.fileName || ""
                fileSizeBytes: model.fileSizeBytes || 0
                fileState: model.fileState || "missing"
                fileProgress: model.fileProgress || 0
                fileWidth: model.fileWidth || 0
                fileHeight: model.fileHeight || 0
                fileThumb: model.fileThumb || ""
                onDownloadRequested: chatView.fileDownloadRequested(model.messageId)
                onSaveRequested: chatView.fileSaveRequested(model.messageId)
                onEditRequested: chatView.editRequested(model.messageId, model.content)
                onDeleteRequested: chatView.deleteRequested(model.messageId)
            }
        }

        // 审查修复：移除 add 过渡动画。群聊下消息/系统消息/同步批量插入频繁，
        // 动画运行中 delegate 被 clear() 销毁会留下悬空通知端点（崩溃于
        // QQmlNotifierEndpoint::disconnect），动画收益小于稳定性风险

        // 内容高度变化时若已贴近底部则自动跟随（新消息到达场景）
        onContentHeightChanged: {
            if (stayAtBottom && contentHeight > height) {
                programmaticScroll = true
                contentY = contentHeight - height - Theme.spacingSmall * 2
                Qt.callLater(function() { chatView.programmaticScroll = false })
            }
        }

        // 用户手动滚动时暂停自动贴底，避免新消息把视图拽回去
        onMovementStarted: {
            if (!chatView.programmaticScroll) {
                chatView.stayAtBottom = false
            }
        }
    }

    // 是否保持贴底（打开会话/发送或接收消息后置 true）
    property bool stayAtBottom: false
    // 程序化滚动标志（区分用户手动滚动）
    property bool programmaticScroll: false

    // 滚动到底部定时器：等待新 delegate 完成布局后再滚动
    Timer {
        id: scrollTimer
        interval: 50
        repeat: false
        onTriggered: {
            chatView.stayAtBottom = true
            chatView.programmaticScroll = true
            if (messageListView.contentHeight > messageListView.height) {
                messageListView.contentY = messageListView.contentHeight - messageListView.height - Theme.spacingSmall * 2
            } else {
                messageListView.contentY = 0
            }
            Qt.callLater(function() { chatView.programmaticScroll = false })
        }
    }

    // 消息输入框
    MessageInput {
        id: messageInput
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        visible: hasConversation
        onMessageSent: function(text) {
            chatView.sendMessage(text)
        }
        // M8.2: 选定附件后上转，上传进度横幅由 MainPage 统一展示
        onAttachmentSelected: function(filePath) {
            chatView.attachmentSelected(filePath)
        }
    }

    // 空状态
    ColumnLayout {
        anchors.centerIn: parent
        spacing: Theme.spacingMedium
        visible: !hasConversation

        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            width: 80; height: 80
            radius: 40
            color: Theme.primaryLightColor

            Label {
                anchors.centerIn: parent
                text: "💬"
                font.pixelSize: 32
            }
        }

        Label {
            Layout.alignment: Qt.AlignHCenter
            text: "选择一个会话开始聊天"
            font.pixelSize: Theme.fontSizeLarge
            color: Theme.textSecondary
        }

        Label {
            Layout.alignment: Qt.AlignHCenter
            text: "或搜索用户发起新对话"
            font.pixelSize: Theme.fontSizeSmall
            color: Theme.textTertiary
        }
    }

    // ── 时间格式化辅助 ──
    function parseDate(createdAt) {
        var d = new Date(createdAt)
        if (isNaN(d.getTime())) {
            d = new Date()
        }
        return d
    }

    function formatTime(createdAt) {
        var d = parseDate(createdAt)
        var hh = d.getHours()
        var mm = d.getMinutes()
        return (hh < 10 ? "0" : "") + hh + ":" + (mm < 10 ? "0" : "") + mm
    }

    function dateKey(createdAt) {
        var d = parseDate(createdAt)
        return d.getFullYear() + "-" + d.getMonth() + "-" + d.getDate()
    }

    function formatDividerText(createdAt) {
        var d = parseDate(createdAt)
        var now = new Date()
        var today = new Date(now.getFullYear(), now.getMonth(), now.getDate())
        var that = new Date(d.getFullYear(), d.getMonth(), d.getDate())
        var diffDays = Math.round((today.getTime() - that.getTime()) / 86400000)
        if (diffDays === 0) return "今天"
        if (diffDays === 1) return "昨天"
        return d.getFullYear() + "年" + (d.getMonth() + 1) + "月" + d.getDate() + "日"
    }

    // M7a: 群系统消息结构化正文转可读文本（与服务端约定 event 取值）
    function systemMessageText(content) {
        try {
            var obj = JSON.parse(content)
            if (obj && obj.event) {
                switch (obj.event) {
                    case "group_created": return "创建了群组"
                    case "member_added": return "新成员加入群聊"
                    case "member_removed": return "成员被移出群聊"
                    case "member_left": return "成员退出了群聊"
                    case "owner_transferred": return "群主已转让"
                    default: return content
                }
            }
        } catch (e) {
            // 非 JSON 正文直接展示原文
        }
        return content
    }

    // 若与上一条消息不在同一天，先插入日期分隔线
    function ensureDivider(createdAt) {
        var key = dateKey(createdAt)
        var lastKey = ""
        for (var i = msgModel.count - 1; i >= 0; i--) {
            var item = msgModel.get(i)
            if (!item.isDivider) {
                lastKey = item.dateKeyStr || ""
                break
            }
        }
        if (lastKey !== key) {
            msgModel.append({
                isDivider: true,
                dividerText: formatDividerText(createdAt),
                dateKeyStr: key,
                messageId: 0, clientMessageId: "", senderId: 0,
                senderUsername: "", content: "", contentType: "text",
                createdAt: "", displayTime: "", status: "", isMine: false,
                undecryptable: false,
                // M8.2: ListModel 要求各条目角色一致，分隔线也带上文件字段
                isFileMessage: false, fileName: "", fileSizeBytes: 0,
                fileSha256: "", fileState: "missing", fileProgress: 0,
                fileWidth: 0, fileHeight: 0, fileThumb: ""
            })
        }
    }

    function makeMessageEntry(msg) {
        return {
            isDivider: false,
            dividerText: "",
            dateKeyStr: dateKey(msg.createdAt || ""),
            messageId: msg.messageId || 0,
            clientMessageId: msg.clientMessageId || "",
            senderId: msg.senderId || 0,
            senderUsername: msg.senderUsername || "",
            content: msg.content || "",
            contentType: msg.contentType || "text",
            createdAt: msg.createdAt || "",
            displayTime: formatTime(msg.createdAt || ""),
            status: msg.status || "",
            isMine: (msg.senderId == chatView.myUserId),
            // M6: 无法解密的端到端加密消息显示占位样式
            undecryptable: msg.undecryptable === true,
            // M9 特性栈：编辑/删除状态
            edited: msg.edited === true,
            deleted: msg.deleted === true,
            // M8.2: 文件消息的脱敏展示字段（密钥与清单正文不经 QML）。
            // fileState 由本地缓存情况初始化，下载进度由引擎信号推进
            isFileMessage: msg.isFileMessage === true,
            fileName: msg.fileName || "",
            fileSizeBytes: msg.fileSizeBytes || 0,
            fileSha256: msg.fileSha256 || "",
            fileState: msg.isFileMessage === true
                       ? (fileTransferAvailable(msg.fileSha256, msg.fileCipherSize) ? "available" : "missing")
                       : "missing",
            fileProgress: 0,
            // M8.3: 图片尺寸与内联缩略图（base64 JPEG，不含密钥）
            fileWidth: msg.fileWidth || 0,
            fileHeight: msg.fileHeight || 0,
            fileThumb: msg.fileThumb || ""
        }
    }

    // M8.2: 本地密文缓存是否已就绪（引擎以密文摘要为缓存键）
    function fileTransferAvailable(sha256, cipherSize) {
        if (typeof fileTransfer === "undefined" || !sha256 || sha256.length === 0) {
            return false
        }
        return fileTransfer.isCached(sha256, cipherSize || 0)
    }

    // M8.2: 更新某条消息的附件状态与下载进度
    function updateFileState(messageId, state) {
        for (var i = 0; i < msgModel.count; i++) {
            if (msgModel.get(i).messageId === messageId) {
                msgModel.setProperty(i, "fileState", state)
                return
            }
        }
    }

    function updateFileProgress(messageId, progress) {
        for (var i = 0; i < msgModel.count; i++) {
            if (msgModel.get(i).messageId === messageId) {
                msgModel.setProperty(i, "fileProgress", progress)
                return
            }
        }
    }

    // ── 公共方法 ──
    function setMessages(messages) {
        msgModel.clear()
        for (var i = 0; i < messages.length; i++) {
            var msg = messages[i]
            ensureDivider(msg.createdAt || "")
            msgModel.append(makeMessageEntry(msg))
        }
        scrollToBottom()
    }

    function appendMessage(msg) {
        ensureDivider(msg.createdAt || "")
        msgModel.append(makeMessageEntry(msg))
        scrollToBottom()
    }

    // M4.5: 乐观插入“发送中”消息（本地立即展示）
    function appendOptimisticMessage(clientMessageId, content) {
        var now = new Date().toISOString()
        ensureDivider(now)
        msgModel.append({
            isDivider: false,
            dividerText: "",
            dateKeyStr: dateKey(now),
            messageId: 0,
            clientMessageId: clientMessageId,
            senderId: chatView.myUserId,
            senderUsername: "",
            content: content,
            contentType: "text",
            createdAt: now,
            displayTime: formatTime(now),
            status: "sending",
            isMine: true,
            undecryptable: false
        })
        scrollToBottom()
    }

    // M4.5: 服务端确认后，以幂等键定位乐观消息并更新为已发送
    function confirmOptimisticMessage(clientMessageId, messageId) {
        for (var i = 0; i < msgModel.count; i++) {
            var item = msgModel.get(i)
            if (!item.isDivider && item.clientMessageId === clientMessageId) {
                msgModel.setProperty(i, "messageId", messageId)
                msgModel.setProperty(i, "status", "sent")
                return
            }
        }
    }

    // M4.5: 收到 MessageStatusUpdate 推送后更新气泡状态
    function updateMessageStatus(messageId, status) {
        for (var i = 0; i < msgModel.count; i++) {
            var item = msgModel.get(i)
            // 使用 == 兼容 C++ qint64 经 JSON 传递到 QML 后可能为 string/number 的情况
            if (!item.isDivider && item.messageId == messageId) {
                msgModel.setProperty(i, "status", status)
                return
            }
        }
    }

    // M9 特性栈：编辑消息后更新气泡内容并标记“已编辑”
    function updateMessageContent(messageId, content) {
        for (var i = 0; i < msgModel.count; i++) {
            var item = msgModel.get(i)
            if (!item.isDivider && item.messageId == messageId) {
                msgModel.setProperty(i, "content", content)
                msgModel.setProperty(i, "undecryptable", false)
                msgModel.setProperty(i, "edited", true)
                return
            }
        }
    }

    // M9 特性栈：删除消息后本地置灰占位
    function markMessageDeleted(messageId) {
        for (var i = 0; i < msgModel.count; i++) {
            var item = msgModel.get(i)
            if (!item.isDivider && item.messageId == messageId) {
                msgModel.setProperty(i, "content", "")
                msgModel.setProperty(i, "deleted", true)
                return
            }
        }
    }

    // M4.5: 返回当前列表中最后一条对方消息的 ID（用于已读回执）
    function lastIncomingMessageId() {
        for (var i = msgModel.count - 1; i >= 0; i--) {
            var item = msgModel.get(i)
            if (!item.isDivider && !item.isMine && item.messageId > 0) {
                return item.messageId
            }
        }
        return 0
    }

    function scrollToBottom() {
        scrollTimer.restart()
    }

    function clearMessages() {
        chatView.stayAtBottom = false
        msgModel.clear()
    }
}
