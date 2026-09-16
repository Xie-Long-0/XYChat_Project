import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

import "../theme"
import "../components"
import "../dialogs"

Rectangle {
    id: mainPage
    color: Theme.windowBackground

    signal searchUsersRequested(string query)
    signal addContactRequested(int userId)
    signal loadConversationsRequested()
    signal loadMessagesRequested(int conversationId, int afterId)
    signal sendMessageRequested(int peerUserId, string content)
    signal logoutRequested()
    signal loadContactsRequested()
    signal createGroupRequested(string name, var memberIds)
    signal inviteGroupMembersRequested(int conversationId, var userIds)
    signal leaveGroupRequested(int conversationId)
    signal kickGroupMemberRequested(int conversationId, int userId)
    signal getGroupInfoRequested(int conversationId)
    signal sendGroupMessageRequested(int conversationId, string content)
    signal setConversationPrefsRequested(int conversationId, bool pinned, bool muted)
    signal editMessageRequested(int conversationId, int peerUserId, int messageId, string content)
    signal deleteMessageRequested(int messageId)
    signal deleteConversationRequested(int conversationId)
    signal markConversationReadRequested(int conversationId)
    signal typingRequested(int conversationId, bool typing)

    property int myUserId: 0
    property string myUsername: ""
    property int currentConversationId: 0
    property int currentPeerUserId: 0
    property string currentPeerUsername: ""
    property string currentConversationType: "private"
    // P4.1: 上传任务 token 集合（token -> true）。下载 token 在 downloadTokens，
    // 但缓存命中的下载会在 download() 内同步完成（先于 token 登记），故上传侧
    // 单独登记，以便 taskFinished 时正确区分上传/下载并弹出“文件已发送”
    property var uploadTokens: ({})
    // P4.1: 每个传输任务的进度/相位快照（token -> {phase, progress}），由
    // taskProgress 增量刷新；tasks 属性仅在任务增删（tasksChanged）时刷新条目
    // 集合，故进度比值在此单独维护，整表重新赋值以触发横幅 delegate 绑定重算
    property var taskProgressMap: ({})
    property int pendingSaveMessageId: 0
    property string fileNotice: ""
    property var downloadTokens: ({})
    property int previewMessageId: 0
    property int playMessageId: 0
    property int currentGroupMemberCount: 0
    property string searchMode: "chat"
    // M11A A4: 草稿缓存（conversationId -> text），切换会话时保存/恢复输入框文本
    // 内存级：退出应用后不保留（与规划一致）
    property var drafts: ({})
    // M11A A5: 待滚动到的消息 ID（搜索跳转时设置，消息加载完成后执行滚动）
    property int pendingScrollMessageId: 0

    // P4.1: 传输横幅改为 Repeater over fileTransfer.tasks，多任务并发时每个任务
    // 独立显示“文件名 · 相位 进度% + 进度条 + 取消”。横幅置于内容区上方全宽
    //（此前误作 RowLayout 兄弟节点，可见时会挤占聊天区一半宽度）
    Rectangle {
        id: transferBanner
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: visible ? bannerColumn.implicitHeight + Theme.spacingSmall * 2 : 0
        visible: (typeof fileTransfer !== "undefined" && fileTransfer.tasks.length > 0)
                 || mainPage.fileNotice.length > 0
        color: Theme.inputBackground
        radius: Theme.radiusMedium

        Behavior on height { NumberAnimation { duration: Theme.animationFast } }

        ColumnLayout {
            id: bannerColumn
            anchors.fill: parent
            anchors.leftMargin: Theme.spacingMedium
            anchors.rightMargin: Theme.spacingMedium
            anchors.topMargin: Theme.spacingSmall
            anchors.bottomMargin: Theme.spacingSmall
            spacing: Theme.spacingSmall

            Repeater {
                model: typeof fileTransfer !== "undefined" ? fileTransfer.tasks : []

                delegate: RowLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: Theme.spacingSmall

                    Label {
                        Layout.fillWidth: true
                        text: {
                            var st = mainPage.taskProgressMap[modelData.token]
                            var phase = (st && st.phase.length > 0) ? st.phase
                                        : (modelData.phase.length > 0 ? modelData.phase : "准备中")
                            var pct = st ? Math.round(st.progress * 100) : 0
                            var name = modelData.fileName.length > 0 ? modelData.fileName
                                       : (modelData.isUpload ? "上传文件" : "下载文件")
                            return name + " · " + mainPage.phaseText(phase) + " " + pct + "%"
                        }
                        elide: Text.ElideMiddle
                        font.pixelSize: Theme.fontSizeCaption
                        color: Theme.textSecondary
                    }

                    ProgressBar {
                        Layout.preferredWidth: 120
                        from: 0
                        to: 1
                        value: {
                            var st = mainPage.taskProgressMap[modelData.token]
                            return st ? st.progress : 0
                        }
                    }

                    AppButton {
                        text: "取消"
                        variant: "secondary"
                        onClicked: fileTransfer.cancelTask(modelData.token)
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                visible: mainPage.fileNotice.length > 0
                spacing: Theme.spacingSmall

                Label {
                    Layout.fillWidth: true
                    text: mainPage.fileNotice
                    elide: Text.ElideMiddle
                    font.pixelSize: Theme.fontSizeCaption
                    color: Theme.textSecondary
                }

                AppButton {
                    text: "关闭"
                    variant: "secondary"
                    onClicked: mainPage.fileNotice = ""
                }
            }
        }
    }

    RowLayout {
        anchors.top: transferBanner.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        spacing: 0

        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: Theme.sidebarWidth
            color: Theme.sidebarBackground

            Rectangle {
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: 1
                color: Theme.separatorColor
            }

            ConversationList {
                id: convList
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: userBar.top

                onConversationClicked: function(index) {
                    var conv = convList.getConversation(index)
                    if (conv) mainPage.openConversation(conv)
                }
                onSearchClicked: {
                    searchMode = "chat"
                    searchDialog.open()
                }
                onMessageSearchClicked: localSearchDialog.open()
                onRefreshClicked: loadConversationsRequested()
                onCreateGroupClicked: createGroupDialog.open()
                onConversationPrefsRequested: function(conversationId, pinned, muted) {
                    mainPage.setConversationPrefsRequested(conversationId, pinned, muted)
                }
                onMarkReadRequested: function(conversationId) {
                    mainPage.markConversationReadRequested(conversationId)
                }
                onDeleteConversationRequested: function(conversationId) {
                    // P3.1: 会话整表删除——弹确认对话框（会话删除文案）
                    confirmDeleteDialog.messageId = 0
                    confirmDeleteDialog.conversationId = conversationId
                    confirmDeleteDialog.titleText = "删除会话"
                    confirmDeleteDialog.bodyText = "删除后该会话及其所有消息将永久消失，此操作不可撤销。是否继续？"
                    confirmDeleteDialog.open()
                }
            }

            Rectangle {
                id: userBar
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 52
                color: Theme.sidebarBackground

                Rectangle {
                    anchors.top: parent.top
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

                    Avatar {
                        userId: mainPage.myUserId
                        name: mainPage.myUsername
                        size: Theme.avatarSizeSmall
                    }

                    Label {
                        Layout.fillWidth: true
                        text: mainPage.myUsername
                        font.pixelSize: Theme.fontSizeBody
                        font.weight: Font.DemiBold
                        color: Theme.textPrimary
                        elide: Text.ElideRight
                    }

                    // M11A: 设置按钮（齿轮图标）
                    Rectangle {
                        id: settingsBtn
                        width: 36; height: 36
                        radius: 18
                        color: settingsMouse.containsMouse ? Theme.hoverColor : "transparent"

                        MouseArea {
                            id: settingsMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: settingsDialog.open()
                        }

                        Icon {
                            anchors.centerIn: parent
                            name: "settings"
                            size: 16
                            iconColor: Theme.textSecondary
                        }
                    }

                    Rectangle {
                        id: logoutBtn
                        width: 36; height: 36
                        radius: 18
                        color: logoutMouse.containsMouse ? Theme.hoverColor : "transparent"

                        MouseArea {
                            id: logoutMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: mainPage.logoutRequested()
                        }

                        Icon {
                            anchors.centerIn: parent
                            name: "logout"
                            size: 16
                            iconColor: Theme.textSecondary
                        }
                    }
                }
            }
        }

        ChatView {
            id: chatView
            Layout.fillWidth: true
            Layout.fillHeight: true

            myUserId: mainPage.myUserId
            peerUserId: mainPage.currentPeerUserId
            conversationId: mainPage.currentConversationId
            isGroup: mainPage.currentConversationType === "group"
            groupMemberCount: mainPage.currentGroupMemberCount

            onSendMessage: function(content) {
                if (mainPage.currentConversationType === "group") {
                    if (mainPage.currentConversationId > 0)
                        mainPage.sendGroupMessageRequested(mainPage.currentConversationId, content)
                } else if (mainPage.currentPeerUserId > 0) {
                    mainPage.sendMessageRequested(mainPage.currentPeerUserId, content)
                }
            }
            onGroupInfoRequested: {
                if (mainPage.currentConversationId > 0)
                    mainPage.getGroupInfoRequested(mainPage.currentConversationId)
            }
            onEditRequested: function(messageId, content) {
                editDialog.openFor(messageId, content)
            }
            onDeleteRequested: function(messageId) {
                confirmDeleteDialog.messageId = messageId
                confirmDeleteDialog.open()
            }
            // P2.2: failed 气泡重发——先移除失败气泡，再按当前会话类型重发
            // （经 MainWindow 走 networkManager.sendMessage，会新建“发送中”气泡）
            onResendRequested: function(clientMessageId, content) {
                chatView.removeOptimisticMessage(clientMessageId)
                if (mainPage.currentConversationType === "group") {
                    if (mainPage.currentConversationId > 0)
                        mainPage.sendGroupMessageRequested(mainPage.currentConversationId, content)
                } else if (mainPage.currentPeerUserId > 0) {
                    mainPage.sendMessageRequested(mainPage.currentPeerUserId, content)
                }
            }
            onCopyRequested: function(content) {
                mainPage.copyToClipboard(content)
            }
            // P3.2: 转发“正在输入”信号（需已有会话 ID；新会话首条消息前无 ID，跳过）
            onTypingSignal: function(typing) {
                if (mainPage.currentConversationId > 0)
                    mainPage.typingRequested(mainPage.currentConversationId, typing)
            }
            onAttachmentSelected: function(fileUrl) {
                if (typeof fileTransfer === "undefined" || !fileTransfer.enabled) {
                    mainPage.fileNotice = "服务端未开启文件传输能力"
                    return
                }
                var isGroupChat = mainPage.currentConversationType === "group"
                var convId = isGroupChat ? mainPage.currentConversationId : 0
                var peerId = isGroupChat ? 0 : mainPage.currentPeerUserId
                if (convId <= 0 && peerId <= 0) {
                    mainPage.fileNotice = "请先选择一个会话"
                    return
                }
                mainPage.fileNotice = ""
                // P4.1: 上传任务经 fileTransfer.tasks 自动进横幅（tasksChanged 触发），
                // 此处仅登记 token 以便 taskFinished 时识别为上传
                var tk = fileTransfer.uploadAndSend(fileUrl, convId, peerId)
                var u = mainPage.uploadTokens
                u[tk] = true
                mainPage.uploadTokens = u
            }
            onFileDownloadRequested: function(messageId) {
                if (typeof fileTransfer === "undefined") return
                chatView.updateFileState(messageId, "downloading")
                chatView.updateFileProgress(messageId, 0)
                var token = fileTransfer.download(messageId)
                var map = mainPage.downloadTokens
                map[token] = messageId
                mainPage.downloadTokens = map
                if (fileTransfer.isMessageFileAvailable(messageId)) {
                    chatView.updateFileState(messageId, "available")
                    chatView.updateFileProgress(messageId, 1.0)
                    delete mainPage.downloadTokens[token]
                }
            }
            onFileSaveRequested: function(messageId) {
                mainPage.pendingSaveMessageId = messageId
                saveFileDialog.messageId = messageId
                saveFileDialog.open()
            }
            onFilePreviewRequested: function(messageId) {
                mainPage.previewMessageId = messageId
                imagePreviewDialog.open()
            }
            onFilePlayRequested: function(messageId) {
                if (typeof mediaPlayer === "undefined") return
                if (typeof fileTransfer !== "undefined"
                    && !fileTransfer.isMessageFileAvailable(messageId)) {
                    mainPage.fileNotice = "文件未就绪，请先下载后再播放"
                    return
                }
                mainPage.playMessageId = messageId
                mediaPlaybackDialog.open()
                if (!mediaPlayer.play(messageId)) {
                    mainPage.fileNotice = "无法播放：文件未就绪、不是音视频，或缓存损坏"
                    mediaPlaybackDialog.close()
                    mainPage.playMessageId = 0
                }
            }
        }
    }

    SearchDialog {
        id: searchDialog
        onSearchRequested: function(query) { mainPage.searchUsersRequested(query) }
        onOpenChatRequested: function(userId, username) { mainPage.openChatWithUser(userId, username) }
        onAddContactRequested: function(userId) { mainPage.addContactRequested(userId) }
    }

    CreateGroupDialog {
        id: createGroupDialog
        onCreateRequested: function(name, memberIds) { mainPage.createGroupRequested(name, memberIds) }
        onLoadContactsRequested: { mainPage.loadContactsRequested() }
    }

    GroupInfoDialog {
        id: groupInfoDialog
        myUserId: mainPage.myUserId
        onInviteClicked: {
            inviteDialog.conversationId = mainPage.currentConversationId
            inviteDialog.open()
        }
        onLeaveRequested: function(conversationId) { mainPage.leaveGroupRequested(conversationId) }
        onKickRequested: function(conversationId, userId) { mainPage.kickGroupMemberRequested(conversationId, userId) }
        onRefreshRequested: function(conversationId) { mainPage.getGroupInfoRequested(conversationId) }
    }

    InviteDialog {
        id: inviteDialog
        onSearchRequested: function(query) {
            mainPage.searchMode = "invite"
            mainPage.searchUsersRequested(query)
        }
        onInviteRequested: function(conversationId, userIds) { mainPage.inviteGroupMembersRequested(conversationId, userIds) }
        onClosed: mainPage.searchMode = "chat"
    }

    EditMessageDialog {
        id: editDialog
        onSaveRequested: function(messageId, newContent) {
            var peer = mainPage.currentConversationType === "group" ? 0 : mainPage.currentPeerUserId
            mainPage.editMessageRequested(mainPage.currentConversationId, peer, messageId, newContent)
        }
    }

    ConfirmDeleteDialog {
        id: confirmDeleteDialog
        onConfirmed: function(messageId) { mainPage.deleteMessageRequested(messageId) }
        onConversationConfirmed: function(conversationId) { mainPage.deleteConversationRequested(conversationId) }
    }

    ImagePreviewDialog {
        id: imagePreviewDialog
        messageId: mainPage.previewMessageId
        messageInfo: mainPage.previewMessageId > 0 ? chatView.getMessageById(mainPage.previewMessageId) : null
        onSaveRequested: function(msgId) {
            mainPage.pendingSaveMessageId = msgId
            saveFileDialog.messageId = msgId
            saveFileDialog.open()
        }
    }

    MediaPlaybackDialog {
        id: mediaPlaybackDialog
        messageId: mainPage.playMessageId
        messageInfo: mainPage.playMessageId > 0 ? chatView.getMessageById(mainPage.playMessageId) : null
    }

    SaveFileDialog {
        id: saveFileDialog
        onSaveAccepted: function(msgId, fileUrl) {
            if (typeof fileTransfer === "undefined") {
                mainPage.pendingSaveMessageId = 0
                return
            }
            var path = fileTransfer.toLocalPath(fileUrl)
            if (path.length === 0) {
                mainPage.fileNotice = "无法解析所选路径"
                mainPage.pendingSaveMessageId = 0
                return
            }
            // P4.3: saveToFile 改为异步（返回 token），进度经传输横幅展示，
            // 结果经 onFileSaved（成功）/onTaskFailed（失败）信号反馈
            mainPage.fileNotice = ""
            fileTransfer.saveToFile(msgId, fileUrl)
            mainPage.pendingSaveMessageId = 0
        }
    }

    // M11A: 设置对话框
    SettingsDialog {
        id: settingsDialog
        onClearCacheRequested: {
            if (typeof fileTransfer !== "undefined") {
                var removed = fileTransfer.clearCache()
                mainPage.fileNotice = "已清理 " + removed + " 个缓存文件"
            }
        }
    }

    // M11A A5: 本地消息搜索对话框
    LocalSearchDialog {
        id: localSearchDialog
        onJumpToMessageRequested: function(conversationId, messageId) {
            // 先切换到目标会话，设置待滚动消息 ID（消息加载完成后执行滚动）
            var idx = convList.findIndexByConversationId(conversationId)
            if (idx < 0) {
                mainPage.fileNotice = "该消息所在会话已不在列表中"
                return
            }
            var conv = convList.getConversation(idx)
            if (conv) {
                mainPage.pendingScrollMessageId = messageId
                mainPage.openConversation(conv)
            }
        }
    }

    Connections {
        target: typeof fileTransfer !== "undefined" ? fileTransfer : null

        function onTaskProgress(token, phase, done, total) {
            var ratio = total > 0 ? done / total : 0
            // P4.1: 按 token 增量刷新进度快照，驱动横幅对应任务行（多任务并发）
            var pm = mainPage.taskProgressMap
            pm[token] = { phase: phase, progress: ratio }
            mainPage.taskProgressMap = pm
            var mid = mainPage.downloadTokens[token]
            if (mid !== undefined) chatView.updateFileProgress(mid, ratio)
        }

        function onTaskFinished(token) {
            var pm = mainPage.taskProgressMap
            delete pm[token]
            mainPage.taskProgressMap = pm
            var mid = mainPage.downloadTokens[token]
            if (mid !== undefined) {
                chatView.updateFileState(mid, "available")
                chatView.updateFileProgress(mid, 1.0)
                delete mainPage.downloadTokens[token]
            }
            if (mainPage.uploadTokens[token] !== undefined) {
                var u = mainPage.uploadTokens
                delete u[token]
                mainPage.uploadTokens = u
                mainPage.fileNotice = "文件已发送"
            }
        }

        function onTaskFailed(token, error) {
            var pm = mainPage.taskProgressMap
            delete pm[token]
            mainPage.taskProgressMap = pm
            var mid = mainPage.downloadTokens[token]
            if (mid !== undefined) {
                chatView.updateFileState(mid, "missing")
                delete mainPage.downloadTokens[token]
            }
            if (mainPage.uploadTokens[token] !== undefined) {
                var u = mainPage.uploadTokens
                delete u[token]
                mainPage.uploadTokens = u
            }
            mainPage.fileNotice = error
        }

        function onDownloadStateChanged(messageId, state) {
            chatView.updateFileState(messageId, state)
        }

        function onFileSaved(messageId, path) {
            mainPage.fileNotice = "已保存到 " + path
        }

        function onCacheCleared(removedCount) {
            mainPage.fileNotice = "已清理 " + removedCount + " 个缓存文件"
        }
    }

    Connections {
        target: typeof mediaPlayer !== "undefined" ? mediaPlayer : null

        function onErrorOccurred(error) {
            mainPage.fileNotice = "播放失败：" + error
            if (typeof mediaPlayer !== "undefined") mediaPlayer.stop()
            if (mediaPlaybackDialog.visible) mediaPlaybackDialog.close()
            mainPage.playMessageId = 0
        }
    }

    // M11A A3: 绑定当前活动会话到托盘管理器（抑制该会话的桌面通知）
    Binding {
        target: typeof trayManager !== "undefined" ? trayManager : null
        property: "activeConversation"
        value: mainPage.currentConversationId
        when: target !== null
    }

    // M11A A3: 点击桌面通知时跳转到对应会话
    Connections {
        target: typeof trayManager !== "undefined" ? trayManager : null

        function onNotificationClicked(conversationId) {
            var idx = convList.findIndexByConversationId(conversationId)
            if (idx >= 0) {
                var conv = convList.getConversation(idx)
                if (conv) mainPage.openConversation(conv)
            }
        }
    }

    // P2.2: 剪贴板助手。QML 无直接剪贴板 API，借只读 TextEdit 的
    // selectAll + copy 实现“复制消息正文”（隐藏、零尺寸，不参与布局与交互）
    TextEdit {
        id: clipboardHelper
        visible: false
        width: 0
        height: 0
        readOnly: true
    }

    function openConversation(conv) {
        // M11A A4: 保存当前会话的草稿（切换前）
        if (currentConversationId > 0) {
            var d = drafts
            d[currentConversationId] = chatView.inputText
            drafts = d
        }

        currentConversationId = conv.conversationId
        currentConversationType = conv.type || "private"
        if (currentConversationType === "group") {
            currentPeerUserId = 0
            currentPeerUsername = ""
            currentGroupMemberCount = conv.memberCount || 0
            chatView.peerUsername = conv.name || "未命名群组"
            chatView.chatTitle = conv.name || "未命名群组"
        } else {
            currentPeerUserId = conv.peerUserId
            currentPeerUsername = conv.peerUsername
            currentGroupMemberCount = 0
            chatView.peerUsername = conv.peerUsername
            chatView.chatTitle = conv.peerUsername
        }
        chatView.hasConversation = true
        chatView.clearMessages()
        // P2.3: 注入未读数，供异步同步返回后 setMessages 放置未读分隔线
        chatView.unreadCount = conv.unreadCount || 0
        convList.setSelectedByConversationId(conv.conversationId)
        loadMessagesRequested(conv.conversationId, 0)

        // M11A A4: 恢复新会话的草稿（切换后）
        chatView.inputText = drafts[conv.conversationId] || ""
    }

    function openChatWithUser(userId, username) {
        // M11A A4: 保存当前会话的草稿（切换前）
        if (currentConversationId > 0) {
            var d = drafts
            d[currentConversationId] = chatView.inputText
            drafts = d
        }

        var existing = convList.findConversationByPeerId(userId)
        if (existing) {
            openConversation(existing)
            return
        }
        currentConversationId = 0
        currentConversationType = "private"
        currentPeerUserId = userId
        currentPeerUsername = username
        currentGroupMemberCount = 0
        chatView.peerUsername = username
        chatView.chatTitle = username
        chatView.hasConversation = true
        chatView.clearMessages()
        chatView.unreadCount = 0
        convList.selectedIndex = -1
        // M11A A4: 新会话无草稿，清空输入框
        chatView.inputText = ""
    }

    function bindNewConversation(conversationId) {
        if (currentConversationId == 0 && conversationId > 0) {
            currentConversationId = conversationId
            convList.setSelectedByConversationId(conversationId)
        }
    }

    function trackOutgoingMessage(clientMessageId, content) {
        chatView.appendOptimisticMessage(clientMessageId, content)
        if (currentConversationId > 0) {
            var now = new Date()
            var hh = now.getHours()
            var mm = now.getMinutes()
            var timeStr = (hh < 10 ? "0" : "") + hh + ":" + (mm < 10 ? "0" : "") + mm
            convList.updateForNewMessage(currentConversationId, content, timeStr, false)
            // M11A A4: 发送成功后清除该会话的草稿
            var d = drafts
            delete d[currentConversationId]
            drafts = d
        }
    }

    function updateConversations(conversations) {
        convList.updateConversations(conversations)
        if (currentConversationId > 0) {
            convList.setSelectedByConversationId(currentConversationId)
            for (var i = 0; i < conversations.length; i++) {
                var conv = conversations[i]
                if ((conv.conversationId || 0) == currentConversationId) {
                    if ((conv.type || "private") === "group") {
                        currentGroupMemberCount = conv.memberCount || 0
                        chatView.chatTitle = conv.name || "未命名群组"
                    }
                    break
                }
            }
        }
    }

    function updateMessages(messages) {
        chatView.setMessages(messages)
        // M11A A5: 消息加载完成后执行待定的滚动（搜索结果跳转）
        if (pendingScrollMessageId > 0) {
            chatView.scrollToMessage(pendingScrollMessageId)
            pendingScrollMessageId = 0
        }
    }

    function appendMessage(msg) { chatView.appendMessage(msg) }
    function confirmMessage(clientMessageId, messageId) { chatView.confirmOptimisticMessage(clientMessageId, messageId) }
    function updateMessageStatus(messageId, status) { chatView.updateMessageStatus(messageId, status) }

    function updateConversationPreview(message) {
        var now = new Date()
        var hh = now.getHours()
        var mm = now.getMinutes()
        var timeStr = (hh < 10 ? "0" : "") + hh + ":" + (mm < 10 ? "0" : "") + mm
        var contentText = message.contentType === "system"
            ? chatView.systemMessageText(message.content || "")
            : message.content
        var preview = message.senderUsername ? message.senderUsername + ": " + contentText : contentText
        convList.updateForNewMessage(message.conversationId, preview, timeStr, true)
    }

    function lastIncomingMessageId() { return chatView.lastIncomingMessageId() }

    // P4.1: 传输相位短标识转可读中文（横幅任务行展示用）
    function phaseText(phase) {
        switch (phase) {
        case "extracting": return "提取元数据"
        case "hashing": return "计算校验"
        case "creating": return "创建上传"
        case "requesting": return "请求下载"
        case "uploading": return "上传中"
        case "downloading": return "下载中"
        case "completing": return "完成中"
        case "recovering": return "恢复中"
        case "saving": return "保存中"
        case "cancelling": return "取消中"
        case "cached": return "已缓存"
        default: return phase
        }
    }

    function showSearchResults(users) {
        if (searchMode === "invite") inviteDialog.showResults(users)
        else searchDialog.showResults(users)
    }

    function showContacts(contacts) { createGroupDialog.showContacts(contacts) }
    function showGroupInfo(info) { groupInfoDialog.showInfo(info) }

    function resetUi() {
        currentConversationId = 0
        currentPeerUserId = 0
        currentPeerUsername = ""
        currentConversationType = "private"
        currentGroupMemberCount = 0
        searchMode = "chat"
        chatView.hasConversation = false
        chatView.peerUsername = ""
        chatView.chatTitle = ""
        chatView.clearMessages()
        convList.reset()
    }

    function openCreatedGroup(conversationId, name) {
        convList.beginLoading()
        loadConversationsRequested()
        currentConversationId = conversationId
        currentConversationType = "group"
        currentPeerUserId = 0
        currentPeerUsername = ""
        chatView.peerUsername = name
        chatView.chatTitle = name
        chatView.hasConversation = true
        chatView.clearMessages()
        chatView.unreadCount = 0
        loadMessagesRequested(conversationId, 0)
    }

    function closeGroupIfCurrent(conversationId) {
        if (currentConversationId == conversationId) {
            currentConversationId = 0
            currentConversationType = "private"
            currentGroupMemberCount = 0
            chatView.hasConversation = false
            chatView.peerUsername = ""
            chatView.chatTitle = ""
            chatView.clearMessages()
            convList.selectedIndex = -1
        }
        convList.beginLoading()
        loadConversationsRequested()
    }

    function applyConversationPrefs(conversationId, pinned, muted) { convList.applyPrefs(conversationId, pinned, muted) }

    // P3.1: 会话被删除（本端响应或其他成员/设备推送）——从列表移除，
    // 若为当前打开的会话则关闭聊天区
    function handleConversationDeleted(conversationId) {
        convList.removeConversation(conversationId)
        if (currentConversationId == conversationId) {
            currentConversationId = 0
            currentConversationType = "private"
            currentPeerUserId = 0
            currentPeerUsername = ""
            currentGroupMemberCount = 0
            chatView.hasConversation = false
            chatView.peerUsername = ""
            chatView.chatTitle = ""
            chatView.clearMessages()
        }
    }

    function applyMessageEdited(conversationId, messageId, content, editedAt) {
        if (conversationId == currentConversationId) chatView.updateMessageContent(messageId, content)
    }

    function applyMessageDeleted(conversationId, messageId) {
        if (conversationId == currentConversationId) chatView.markMessageDeleted(messageId)
    }

    // P2.2: 复制文本到系统剪贴板（隐藏 TextEdit 选中后 copy）
    function copyToClipboard(text) {
        if (!text || text.length === 0) return
        clipboardHelper.text = text
        clipboardHelper.selectAll()
        clipboardHelper.copy()
        clipboardHelper.deselect()
    }

    // P2.2: 发送失败回调。把当前会话中最早一条“发送中”气泡标记为 failed，
    // 由 MainWindow 在 onMessageSendFailed 时调用（与 Toast 并行）
    function handleSendFailed() {
        chatView.markSendingFailed()
    }

    // P3.2: 收到对方“正在输入”推送（仅当前会话生效，由 MainWindow 过滤）
    function showPeerTyping(userId, username, typing) {
        chatView.setTyping(userId, username, typing)
    }
}
