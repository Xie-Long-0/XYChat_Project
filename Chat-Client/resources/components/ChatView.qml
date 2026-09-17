import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "../theme"
import "ChatText.js" as ChatText

Rectangle {
    id: chatView
    color: Theme.chatBackground

    property string peerUsername: ""
    property string chatTitle: ""
    property bool hasConversation: false
    property int myUserId: 0
    property int peerUserId: 0
    property int conversationId: 0
    property bool isGroup: false
    property int groupMemberCount: 0

    // P2.3: 当前会话未读数（MainPage 打开会话时注入，用于放置未读分隔线）
    property int unreadCount: 0
    // P2.3: 滚动离开底部期间新到达的对方消息数（“跳到底部”FAB 徽标）
    property int newMessageCount: 0

    // P3.2: “正在输入”状态。typingUsers 为 userId -> {username, ts}；
    // typingText 为头部副标题展示文本（空表示无人输入）
    property var typingUsers: ({})
    property string typingText: ""

    // P4.2: messageId -> msgModel 行号索引，令 updateFileState/updateFileProgress
    // 从 O(n) 全表扫描降为 O(1)（大文件多分片进度事件的热点路径）
    property var msgIndex: ({})

    // M11A A4: 暴露输入框文本供 MainPage 草稿保存/恢复
    property alias inputText: messageInput.text

    signal sendMessage(string content)
    signal backClicked()
    signal groupInfoRequested()
    // P3.2: 用户键入（true）或发送/清空（false），转发到 MainPage 发“正在输入”信号
    signal typingSignal(bool typing)
    // M9 特性栈：消息右键菜单操作（转发到 MainPage）
    signal editRequested(int messageId, string content)
    signal deleteRequested(int messageId)
    // P2.2: failed 气泡点击重发（带幂等键与正文）与复制正文（均转发到 MainPage）
    signal resendRequested(string clientMessageId, string content)
    signal copyRequested(string content)
    // M8.2: 文件消息的下载/另存请求（由 MainPage 接到传输引擎）
    signal fileDownloadRequested(int messageId)
    signal fileSaveRequested(int messageId)
    // M8.3: 图片消息的应用内大图预览（由 MainPage 打开预览对话框）
    signal filePreviewRequested(int messageId)
    // M8.3b: 音视频消息的应用内播放（由 MainPage 打开播放器对话框）
    signal filePlayRequested(int messageId)
    // M8.2: 附件选择上转（由 MainPage 按会话类型分流到群聊/私聊，与 sendMessage 一致）。
    // 用 var 保留 QUrl，避免转字符串引入编解码歧义
    signal attachmentSelected(var fileUrl)

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

                Icon {
                    anchors.centerIn: parent
                    name: "back"
                    size: 12
                    iconColor: Theme.textSecondary
                }
            }

            Avatar {
                userId: isGroup ? conversationId : peerUserId
                name: peerUsername
                size: Theme.avatarSizeSmall
                isGroup: chatView.isGroup
                visible: hasConversation
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

                // M7a: 群会话副标题（成员数）；P3.2: 有人输入时优先显示“正在输入”
                Label {
                    visible: isGroup && chatView.typingText.length === 0
                    text: groupMemberCount + " 位成员"
                    font.pixelSize: Theme.fontSizeSmall - 1
                    color: Theme.textTertiary
                }

                // P3.2: “正在输入…”副标题（私聊/群聊通用，主色区分）
                Label {
                    visible: chatView.typingText.length > 0
                    text: chatView.typingText
                    font.pixelSize: Theme.fontSizeSmall - 1
                    color: Theme.primaryColor
                    elide: Text.ElideRight
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

                Icon {
                    anchors.centerIn: parent
                    name: "group"
                    size: 16
                    iconColor: Theme.textSecondary
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

            // 末行的真实布局（创建/高度落地/位置校正）是重算贴底位置的时机：
            // positionViewAtEnd 计算时该行可能尚未创建，用的是估算高度
            onYChanged: chatView.onRowLayoutChanged(index)
            onHeightChanged: chatView.onRowLayoutChanged(index)

            // 日期分隔线
            Item {
                width: parent.width
                height: 32
                visible: model.isDivider && model.isUnreadDivider !== true

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

            // P2.3: 未读消息分隔线（进入会话时标记“从这里开始未读”，主色胶囊区分于日期线）
            Item {
                width: parent.width
                height: 32
                visible: model.isUnreadDivider === true

                Rectangle {
                    anchors.centerIn: parent
                    height: 22
                    width: unreadDividerLabel.implicitWidth + Theme.spacingLarge * 2
                    radius: 11
                    color: Theme.primaryColor

                    Label {
                        id: unreadDividerLabel
                        anchors.centerIn: parent
                        text: "未读消息"
                        font.pixelSize: Theme.fontSizeSmall
                        font.weight: Font.DemiBold
                        color: Theme.textOnPrimary
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
                        text: ChatText.systemMessageText(model.content)
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
                clientMessageId: model.clientMessageId || ""
                edited: model.edited === true
                deleted: model.deleted === true
                // M8.2: 文件消息的脱敏展示与下载/保存交互
                isFileMessage: model.isFileMessage === true
                fileName: model.fileName || ""
                fileMime: model.fileMime || ""
                fileSizeBytes: model.fileSizeBytes || 0
                fileState: model.fileState || "missing"
                fileProgress: model.fileProgress || 0
                fileWidth: model.fileWidth || 0
                fileHeight: model.fileHeight || 0
                fileThumb: model.fileThumb || ""
                fileDurationMs: model.fileDurationMs || 0
                onDownloadRequested: chatView.fileDownloadRequested(model.messageId)
                onSaveRequested: chatView.fileSaveRequested(model.messageId)
                onPreviewRequested: chatView.filePreviewRequested(model.messageId)
                onPlayRequested: chatView.filePlayRequested(model.messageId)
                onEditRequested: chatView.editRequested(model.messageId, model.content)
                onDeleteRequested: chatView.deleteRequested(model.messageId)
                // P2.2: failed 气泡点击重发；复制正文（经 MainPage 剪贴板助手）
                onResendRequested: chatView.resendRequested(model.clientMessageId || "", model.content || "")
                onCopyRequested: chatView.copyRequested(model.content || "")
            }
        }

        // 审查修复：移除 add 过渡动画。群聊下消息/系统消息/同步批量插入频繁，
        // 动画运行中 delegate 被 clear() 销毁会留下悬空通知端点（崩溃于
        // QQmlNotifierEndpoint::disconnect），动画收益小于稳定性风险

        // 内容高度变化时若处于贴底模式则跟随到底部（真正定位延迟到本轮布局结束）
        onContentHeightChanged: chatView.scheduleStick()

        // 视口高度变化（输入框增高/窗口缩放）时同样保持贴底
        onHeightChanged: chatView.scheduleStick()

        // 用户手动滚动时暂停自动贴底，避免新消息把视图拽回去
        // （鼠标拖拽与滚轮都经 Flickable::drag → movementStarting 走这条信号）
        onMovementStarted: {
            if (!chatView.programmaticScroll) {
                chatView.stayAtBottom = false
            }
        }

        // P2.3: 滚动回底部时清零 FAB 新消息徽标（含用户手动与程序化滚动）
        onContentYChanged: {
            chatView.refreshLayoutExtent()
            chatView.clampPhantomY()
            if (chatView.atBottom) {
                chatView.newMessageCount = 0
            }
        }
    }

    // 是否保持贴底（打开会话/发送或接收消息后置 true）
    property bool stayAtBottom: false
    // 程序化滚动标志（区分用户手动滚动）
    property bool programmaticScroll: false

    // 最后一行 delegate 的真实末端（内容坐标），<= 0 表示未知（无消息或末行未创建）。
    // ListView.contentHeight 是按可见行平均高度估算的（见 Qt 源码 qquicklistview.cpp
    // 的 originPosition/lastPosition），未创建的行越多误差越大：实测 31 行时
    // 估计值比真实布局多出 600+px。直接按 contentHeight 定位会落进这段幽灵空白，
    // 表现为“发送后滚出边界、要手动滚回来”，故定位与贴底判定一律用真实末端
    property real layoutExtent: -1
    // contentHeight/contentY 修正护栏（写回会再次触发对应的 changed 信号）
    property bool extentGuard: false
    // 贴底调度合并标志（见 scheduleStick）
    property bool stickScheduled: false

    // 取末行真实末端；末行尚未创建（delegate 正在实例化、还没注册为可见项）
    // 时保留上一次的已知值，避免贴底判定在布局中途退化
    function refreshLayoutExtent() {
        if (msgModel.count === 0) {
            layoutExtent = 0
            return
        }
        var last = messageListView.itemAtIndex(msgModel.count - 1)
        if (last) {
            layoutExtent = last.y + last.height
        }
    }

    // 末行 delegate 布局变化的回调：仅在贴底模式下重算并请求重新贴底
    function onRowLayoutChanged(row) {
        if (!stayAtBottom || row !== msgModel.count - 1) {
            return
        }
        refreshLayoutExtent()
        scheduleStick()
    }

    // 把 contentHeight 压回真实末端，消除估算出来的幽灵可滚动区
    function clampPhantomSpace() {
        if (extentGuard || layoutExtent <= 0) {
            return
        }
        if (messageListView.contentHeight > layoutExtent + 1) {
            extentGuard = true
            messageListView.contentHeight = layoutExtent + 1
            extentGuard = false
        }
    }

    // 拖拽/惯性滚动进入幽灵空白区时拉回真实末端
    function clampPhantomY() {
        if (extentGuard || layoutExtent <= 0) {
            return
        }
        var maxY = Math.max(0, layoutExtent - messageListView.height)
        if (messageListView.contentY > maxY + 1) {
            extentGuard = true
            messageListView.contentY = maxY
            extentGuard = false
        }
    }

    // 贴底调度：ListView 布局过程中（delegate 创建/高度变化等信号回调里）绝不能
    // 直接调 positionViewAtEnd——那是对同一套布局逻辑的重入，会把外层定位算坏
    // （实测：重入会把 contentY 写回 0，最终停在估算末端，仍留越界空白）。
    // 因此所有贴底请求在此合并，延迟到本轮布局结束后的下一个事件循环执行
    function scheduleStick() {
        if (stickScheduled) {
            return
        }
        stickScheduled = true
        Qt.callLater(chatView.applyStick)
    }

    // 贴底执行：先修正真实末端与幽灵空白，再按需重新定位
    function applyStick() {
        stickScheduled = false
        refreshLayoutExtent()
        clampPhantomSpace()
        clampPhantomY()
        if (!stayAtBottom || !hasConversation || msgModel.count === 0) {
            return
        }
        if (messageListView.contentHeight <= messageListView.height + 1) {
            if (messageListView.contentY !== 0) {
                messageListView.contentY = 0
            }
            return
        }
        if (atBottom) {
            return
        }
        var before = messageListView.contentY
        messageListView.positionViewAtEnd()
        // 定位后末行才可能被创建并拿到真实高度：位置有变化就再校正一轮
        if (Math.abs(messageListView.contentY - before) > 0.5) {
            scheduleStick()
        }
    }

    // P2.3: 是否贴近底部（留 2px 容差）。以末行真实末端判定：估算的 contentHeight
    // 会把“已在底部”误判为“尚未到底”，使 FAB 常驻
    readonly property bool atBottom: {
        if (!hasConversation || msgModel.count === 0) {
            return true
        }
        if (layoutExtent <= 0) {
            return messageListView.contentHeight <= messageListView.height + 1
        }
        return messageListView.contentY + messageListView.height >= layoutExtent - 2
    }
    readonly property bool scrolledUp: hasConversation && msgModel.count > 0 && !atBottom

    // 滚动到底部定时器：等新 delegate 完成创建与布局后再执行贴底
    Timer {
        id: scrollTimer
        interval: 50
        repeat: false
        onTriggered: {
            chatView.stayAtBottom = true
            chatView.programmaticScroll = true
            chatView.applyStick()
            Qt.callLater(function() { chatView.programmaticScroll = false })
        }
    }

    // P3.2: “正在输入”过期清理（每秒剔除 5s 无新信号的成员，空则停表）
    Timer {
        id: typingPruneTimer
        interval: 1000
        repeat: true
        running: false
        onTriggered: chatView.pruneTyping()
    }

    // 消息输入框
    MessageInput {
        id: messageInput
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        visible: hasConversation
        maxLength: isGroup ? 16384 : 4096
        onMessageSent: function(text) {
            chatView.sendMessage(text)
            // P3.2: 发送后停止“正在输入”
            chatView.typingSignal(false)
        }
        onAttachmentSelected: function(fileUrl) {
            chatView.attachmentSelected(fileUrl)
        }
        // P3.2: 键入时上报“正在输入”（C++ 侧节流）
        onTypingActivity: chatView.typingSignal(true)
    }

    // P2.3: “跳到底部”悬浮按钮（向上滚动离开底部时出现，右下角，带新消息徽标）
    Rectangle {
        id: jumpToBottomFab
        anchors.right: parent.right
        anchors.bottom: messageInput.top
        anchors.rightMargin: Theme.spacingLarge
        anchors.bottomMargin: Theme.spacingMedium
        width: 44
        height: 44
        radius: 22
        visible: chatView.scrolledUp
        color: fabMouse.containsMouse ? Theme.hoverColor : Theme.inputBackground
        border.width: 1
        border.color: Theme.separatorColor
        scale: visible ? 1 : 0.6
        Behavior on scale {
            NumberAnimation { duration: Theme.animationFast; easing.type: Theme.easingDecelerate }
        }

        Icon {
            anchors.centerIn: parent
            name: "arrow-down"
            size: 20
            iconColor: Theme.textSecondary
        }

        MouseArea {
            id: fabMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: {
                chatView.newMessageCount = 0
                chatView.scrollToBottom()
            }
        }

        // 新消息徽标（滚动离开底部期间到达的对方消息数）
        Rectangle {
            visible: chatView.newMessageCount > 0
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.rightMargin: -2
            anchors.topMargin: -4
            width: Math.max(18, fabBadgeLabel.implicitWidth + 10)
            height: 18
            radius: 9
            color: Theme.unreadBadgeColor
            border.width: 2
            border.color: Theme.chatBackground

            Label {
                id: fabBadgeLabel
                anchors.centerIn: parent
                text: chatView.newMessageCount > 99 ? "99+" : chatView.newMessageCount
                color: Theme.unreadBadgeTextColor
                font.pixelSize: Theme.fontSizeSmall - 1
                font.weight: Font.DemiBold
            }
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

            Icon {
                anchors.centerIn: parent
                name: "chat-bubble"
                size: 32
                iconColor: Theme.primaryColor
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

    // 消息加载中状态（已选会话但消息尚未到达）
    Column {
        anchors.centerIn: parent
        spacing: Theme.spacingMedium
        visible: hasConversation && msgModel.count === 0

        LoadingIndicator {
            anchors.horizontalCenter: parent.horizontalCenter
            size: 24
            running: visible
        }

        Label {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "加载消息..."
            font.pixelSize: Theme.fontSizeMedium
            color: Theme.textTertiary
        }
    }

    // 时间格式化辅助
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

    // 日期分隔线的条目模板（M8.2: ListModel 要求各条目角色一致，分隔线也带上文件字段）
    function dividerEntry(createdAt) {
        return {
            isDivider: true,
            dividerText: formatDividerText(createdAt),
            dateKeyStr: dateKey(createdAt),
            messageId: 0, clientMessageId: "", senderId: 0,
            senderUsername: "", content: "", contentType: "text",
            createdAt: "", displayTime: "", status: "", isMine: false,
            undecryptable: false,
            isFileMessage: false, fileName: "", fileSizeBytes: 0,
            fileSha256: "", fileState: "missing", fileProgress: 0,
            fileWidth: 0, fileHeight: 0, fileThumb: "", fileMime: "",
            fileDurationMs: 0
        }
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
            msgModel.append(dividerEntry(createdAt))
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
            fileMime: msg.fileMime || "",
            fileSizeBytes: msg.fileSizeBytes || 0,
            fileSha256: msg.fileSha256 || "",
            fileState: msg.isFileMessage === true
                       ? (fileTransferAvailable(msg.fileSha256, msg.fileCipherSize) ? "available" : "missing")
                       : "missing",
            fileProgress: 0,
            // M8.3: 图片尺寸与内联缩略图（base64 JPEG，不含密钥）
            fileWidth: msg.fileWidth || 0,
            fileHeight: msg.fileHeight || 0,
            fileThumb: msg.fileThumb || "",
            // M8.3b: 音视频时长（毫秒，不含密钥），UI 据此展示时长标签
            fileDurationMs: msg.fileDurationMs || 0
        }
    }

    // M8.2: 本地密文缓存是否已就绪（引擎以密文摘要为缓存键）
    function fileTransferAvailable(sha256, cipherSize) {
        if (typeof fileTransfer === "undefined" || !sha256 || sha256.length === 0) {
            return false
        }
        return fileTransfer.isCached(sha256, cipherSize || 0)
    }

    // P4.2: 重建 messageId -> 行号索引（结构性变更后调用；O(n) 一次性）
    function rebuildMessageIndex() {
        var index = {}
        for (var i = 0; i < msgModel.count; i++) {
            var item = msgModel.get(i)
            if (!item.isDivider && item.messageId > 0) {
                index[item.messageId] = i
            }
        }
        msgIndex = index
    }

    // P4.2: 经索引 O(1) 定位消息行；索引失效（结构变更未同步）时重建兜底，
    // 命中前用 == 校验行内容，确保绝不会更新到错误的行
    function rowForMessageId(messageId) {
        var row = msgIndex[messageId]
        if (row !== undefined && row >= 0 && row < msgModel.count) {
            var item = msgModel.get(row)
            if (item && !item.isDivider && item.messageId == messageId) {
                return row
            }
        }
        rebuildMessageIndex()
        row = msgIndex[messageId]
        if (row !== undefined && row >= 0 && row < msgModel.count) {
            var retry = msgModel.get(row)
            if (retry && !retry.isDivider && retry.messageId == messageId) {
                return row
            }
        }
        return -1
    }

    // M8.2: 更新某条消息的附件状态与下载进度（P4.2: 经索引 O(1) 定位）
    function updateFileState(messageId, state) {
        var row = rowForMessageId(messageId)
        if (row >= 0) {
            msgModel.setProperty(row, "fileState", state)
        }
    }

    function updateFileProgress(messageId, progress) {
        var row = rowForMessageId(messageId)
        if (row >= 0) {
            msgModel.setProperty(row, "fileProgress", progress)
        }
    }

    // M8.3c: 按 messageId 取一条消息的完整字段（供图预览对话框拼标题/尺寸/
    // 另存为）。未命中返回 null，调用方需自行容错
    function getMessageById(messageId) {
        for (var i = 0; i < msgModel.count; i++) {
            var item = msgModel.get(i)
            if (item.messageId === messageId) {
                return item
            }
        }
        return null
    }

    // M12.3: 消息状态秩（只前进不回退，与 LocalStore::statusRank 同口径）。
    // 刷新页的状态快照可能滞后于已到达的回执推送，直接覆盖会把气泡拉回旧状态
    function statusRankValue(status) {
        switch (status) {
        case "sent": return 1
        case "delivered": return 2
        case "read": return 3
        default: return 0
        }
    }

    // 只在值变化时写回，避免刷新页对未变化的行触发无谓的 dataChanged
    function setRowIfChanged(row, name, value) {
        if (msgModel.get(row)[name] != value) {
            msgModel.setProperty(row, name, value)
        }
    }

    // M12.3: 原地更新已存在的行（保留 delegate 实例与滚动位置）。
    // 不触碰 fileState/fileProgress——那是传输引擎推进的本地状态，不随页数据回退
    function updateMessageRow(row, msg) {
        var cur = msgModel.get(row)
        if (!cur) {
            return
        }
        var content = msg.content || ""
        if (cur.content !== content) {
            msgModel.setProperty(row, "content", content)
        }
        var undecryptable = msg.undecryptable === true
        if (cur.undecryptable !== undecryptable) {
            msgModel.setProperty(row, "undecryptable", undecryptable)
        }
        var edited = msg.edited === true
        if (cur.edited !== edited) {
            msgModel.setProperty(row, "edited", edited)
        }
        var deleted = msg.deleted === true
        if (cur.deleted !== deleted) {
            msgModel.setProperty(row, "deleted", deleted)
        }
        var status = msg.status || ""
        if (statusRankValue(status) > statusRankValue(cur.status)) {
            msgModel.setProperty(row, "status", status)
        }
        setRowIfChanged(row, "isFileMessage", msg.isFileMessage === true)
        setRowIfChanged(row, "fileName", msg.fileName || "")
        setRowIfChanged(row, "fileMime", msg.fileMime || "")
        setRowIfChanged(row, "fileSizeBytes", msg.fileSizeBytes || 0)
        setRowIfChanged(row, "fileSha256", msg.fileSha256 || "")
        setRowIfChanged(row, "fileWidth", msg.fileWidth || 0)
        setRowIfChanged(row, "fileHeight", msg.fileHeight || 0)
        setRowIfChanged(row, "fileThumb", msg.fileThumb || "")
        setRowIfChanged(row, "fileDurationMs", msg.fileDurationMs || 0)
    }

    // M12.3: 插入点是否需要补日期分隔线。向上找到第一条带日期的行（日期分隔线
    // 或消息行，跳过不记日期的未读分隔线），同日即归其现有分隔管；到顶则必须新插
    function needsDividerAt(pos, createdAt) {
        var key = dateKey(createdAt)
        for (var i = pos - 1; i >= 0; i--) {
            var above = msgModel.get(i)
            if (!above.isDivider) {
                return (above.dateKeyStr || "") !== key
            }
            if (above.isUnreadDivider !== true) {
                return above.dateKeyStr !== key
            }
        }
        return true
    }

    // M12.3: 把一条新消息插入其 messageId 序位置（刷新/实时页追加到尾部，
    // 补收的旧页插到前面），并按需维护日期分隔线
    function insertMessageEntry(msg) {
        var id = msg.messageId || 0
        var createdAt = msg.createdAt || ""
        var key = dateKey(createdAt)
        // 定序：最后一条 id 更小的消息之后；没有更小者（补收更旧的历史）置顶
        var pos = 0
        for (var i = msgModel.count - 1; i >= 0; i--) {
            var item = msgModel.get(i)
            if (!item.isDivider && (item.messageId || 0) < id) {
                pos = i + 1
                break
            }
        }
        // 插入点正处于同日日期分隔线之前时，放到线后复用（不重复插线）
        var at = msgModel.get(pos)
        if (at !== undefined && at.isDivider && at.isUnreadDivider !== true
                && at.dateKeyStr === key) {
            msgModel.insert(pos + 1, makeMessageEntry(msg))
            return
        }
        if (needsDividerAt(pos, createdAt)) {
            msgModel.insert(pos, dividerEntry(createdAt))
            pos++
        }
        msgModel.insert(pos, makeMessageEntry(msg))
    }

    // 公共方法
    // M12.3: 增量应用一页消息（本地缓存页与补收/服务端页共用）。旧实现 clear+全量
    // 重建会销毁全部 delegate 并重置滚动位置——打开会话时缓存页先建一次、
    // 服务端页到达再拆一次，正是切会话卡顿的主要来源。现按 messageId 合并：
    // 已存在的行原地更新（不移动、不重建），新消息按 id 序插入；未读分隔线
    // 只在首次填充（模型为空）时放置，后续刷新不再重排
    function setMessages(messages) {
        var firstFill = msgModel.count === 0
        var wasAtBottom = stayAtBottom
        var index = {}
        for (var i = 0; i < msgModel.count; i++) {
            var item = msgModel.get(i)
            if (!item.isDivider && item.messageId > 0) {
                index[item.messageId] = i
            }
        }
        // 两轮处理：先原地更新（不移动行，首轮索引全程有效），再插入新行
        var fresh = []
        for (var j = 0; j < messages.length; j++) {
            var msg = messages[j]
            var id = msg.messageId || 0
            if (id > 0 && index[id] !== undefined) {
                updateMessageRow(index[id], msg)
            } else {
                fresh.push(msg)
            }
        }
        for (var k = 0; k < fresh.length; k++) {
            insertMessageEntry(fresh[k])
        }
        rebuildMessageIndex()
        if (firstFill) {
            newMessageCount = 0
            insertUnreadDivider()
            // 立即置贴底意图：定位本身仍由 scrollToBottom 的合并调度完成，
            // 但紧随其后的服务端页（几毫秒内到达）必须判定为“贴底跟随”，
            // 否则会被当成用户正在翻历史而误报 FAB 新消息徽标
            stayAtBottom = true
            scrollToBottom()
            return
        }
        if (fresh.length > 0) {
            // 贴底则跟随新内容；正在翻历史则保留阅读位置，只累计 FAB 徽标
            if (wasAtBottom) {
                scrollToBottom()
            } else {
                newMessageCount += fresh.length
            }
        }
    }

    // P2.3: 依据会话未读数，在“第一条未读的对方消息”前插入未读分隔线。
    // 从末尾回溯统计对方消息（跳过自己的、系统消息与分隔线），数到第
    // unreadCount 条即为第一条未读消息；不足则置于顶部。用毕清零，仅显示一次
    function insertUnreadDivider() {
        if (unreadCount <= 0) {
            return
        }
        var remaining = unreadCount
        var insertIndex = 0
        for (var i = msgModel.count - 1; i >= 0; i--) {
            var item = msgModel.get(i)
            if (item.isDivider || item.contentType === "system" || item.isMine) {
                continue
            }
            remaining--
            if (remaining === 0) {
                insertIndex = i
                break
            }
        }
        msgModel.insert(insertIndex, {
            isDivider: true,
            isUnreadDivider: true,
            dividerText: "未读消息",
            dateKeyStr: "",
            messageId: 0, clientMessageId: "", senderId: 0,
            senderUsername: "", content: "", contentType: "unread-divider",
            createdAt: "", displayTime: "", status: "", isMine: false,
            undecryptable: false,
            isFileMessage: false, fileName: "", fileSizeBytes: 0,
            fileSha256: "", fileState: "missing", fileProgress: 0,
            fileWidth: 0, fileHeight: 0, fileThumb: "", fileMime: "",
            fileDurationMs: 0
        })
        unreadCount = 0
    }

    function appendMessage(msg) {
        // M12.4: 幂等——同一条消息可能既经实时推送又出现在补收页里，
        // 重复到达时原地更新而不是再插一条
        var existing = (msg.messageId > 0) ? rowForMessageId(msg.messageId) : -1
        if (existing >= 0) {
            updateMessageRow(existing, msg)
            return
        }
        // P2.3: 记录到达前是否贴底。贴底则自动跟随，否则累计 FAB 新消息徽标、保持阅读位置
        var wasAtBottom = stayAtBottom
        ensureDivider(msg.createdAt || "")
        msgModel.append(makeMessageEntry(msg))
        // P4.2: 新消息行加入索引（追加不移动既有行号）
        var appended = msgModel.get(msgModel.count - 1)
        if (appended.messageId > 0) {
            msgIndex[appended.messageId] = msgModel.count - 1
        }
        if (wasAtBottom) {
            scrollToBottom()
        } else {
            newMessageCount++
        }
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
                // P4.2: 乐观消息获得真实 messageId，补入索引
                if (messageId > 0) {
                    msgIndex[messageId] = i
                }
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

    // P2.2: 发送失败时把"发送中"的乐观气泡标记为 failed，供用户点击重发。
    // 带幂等键时只标那一条（消息可能来自已切走的后台会话）；无键时标最早一条。
    // 返回是否命中（无命中则调用方可判断无需额外处理）
    function markSendingFailed(clientMessageId) {
        var wantKey = (clientMessageId && clientMessageId.length > 0) ? clientMessageId : ""
        for (var i = 0; i < msgModel.count; i++) {
            var item = msgModel.get(i)
            if (item.isDivider || item.status !== "sending") continue
            if (wantKey.length > 0 && item.clientMessageId !== wantKey) continue
            msgModel.setProperty(i, "status", "failed")
            return true
        }
        return false
    }

    // P2.2: 重发前移除指定幂等键的失败乐观气泡，避免与重发新建的气泡重复
    function removeOptimisticMessage(clientMessageId) {
        if (!clientMessageId || clientMessageId.length === 0) {
            return
        }
        for (var i = 0; i < msgModel.count; i++) {
            var item = msgModel.get(i)
            if (!item.isDivider && item.clientMessageId === clientMessageId) {
                msgModel.remove(i)
                // P4.2: 删除使后续行号整体前移，重建索引
                rebuildMessageIndex()
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

    // M12.4: 视图内最后一条已确认消息的 ID。视图被切回时据此做增量补收
    //（afterId > 0 不会回放本地缓存页，视图状态原样保留）
    function lastMessageId() {
        for (var i = msgModel.count - 1; i >= 0; i--) {
            var item = msgModel.get(i)
            if (!item.isDivider && item.messageId > 0) {
                return item.messageId
            }
        }
        return 0
    }

    // P3.2: 设置某成员的“正在输入”状态并重算头部副标题文本
    function setTyping(userId, username, typing) {
        // 忽略自己的 typing（服务端已排除发起者，此处双保险）
        if (userId == chatView.myUserId) {
            return
        }
        var map = chatView.typingUsers
        if (typing) {
            map[userId] = { username: username, ts: Date.now() }
        } else {
            delete map[userId]
        }
        chatView.typingUsers = map
        chatView.recomposeTyping()
        typingPruneTimer.running = Object.keys(chatView.typingUsers).length > 0
    }

    // P3.2: 剔除超过 5s 无新信号的成员（接收端超时自动隐藏）
    function pruneTyping() {
        var now = Date.now()
        var map = chatView.typingUsers
        var changed = false
        for (var key in map) {
            if (now - map[key].ts > 5000) {
                delete map[key]
                changed = true
            }
        }
        if (changed) {
            chatView.typingUsers = map
            chatView.recomposeTyping()
        }
        typingPruneTimer.running = Object.keys(chatView.typingUsers).length > 0
    }

    // P3.2: 组装副标题文本。私聊/单人：“XX 正在输入…”；群聊多人：“XX 等 N 人正在输入…”
    function recomposeTyping() {
        var map = chatView.typingUsers
        var keys = Object.keys(map)
        if (keys.length === 0) {
            chatView.typingText = ""
            return
        }
        var first = map[keys[0]].username
        if (!chatView.isGroup || keys.length === 1) {
            chatView.typingText = first + " 正在输入…"
        } else {
            chatView.typingText = first + " 等 " + keys.length + " 人正在输入…"
        }
    }

    function scrollToBottom() {
        scrollTimer.restart()
    }

    // M11A A5: 滚动到指定消息（本地搜索结果跳转）
    // 返回是否成功定位（消息不在当前列表中时返回 false）
    function scrollToMessage(messageId) {
        var row = rowForMessageId(messageId)
        if (row < 0) {
            return false
        }
        // 取消贴底模式，允许用户查看历史消息
        chatView.stayAtBottom = false
        chatView.programmaticScroll = true
        messageListView.positionViewAtIndex(row, ListView.Center)
        Qt.callLater(function() { chatView.programmaticScroll = false })
        return true
    }
}
