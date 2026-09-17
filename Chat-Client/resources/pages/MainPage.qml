import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

import "../theme"
import "../components"
import "../dialogs"
import "../components/ChatText.js" as ChatText

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
    // M12.4: 切到已打开的会话（未重新加载）时上报，供外层补发已读回执
    signal conversationActivated()

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
    // M12.4: 草稿缓存（viewKey -> 文本）。视图存活期间草稿就在各自输入框里，
    // 此表只承载被淘汰视图的文本，驱逐时转存、下次打开时恢复
    property var drafts: ({})
    // M12.4: 会话视图堆叠——viewKey -> ChatView 实例映射 + 活跃键 + 最近使用
    // 序号。切换会话只改可见性，各视图的消息、滚动位置、草稿、附件进度互不干扰
    property var chatViews: ({})
    property var viewUsage: ({})
    property int viewUseSeq: 0
    property string activeViewKey: ""
    // 同时保留的会话视图上限（超出后按最近使用淘汰最旧的非活动视图）
    readonly property int maxCachedViews: 8
    // M12.4: 在途发送（clientMessageId -> 发起视图 key），发送确认/失败时定位视图。
    // 失败信号不带幂等键，只能靠它判断"在途发送是否只来自唯一视图"
    property var pendingSentKeys: ({})
    // M12.4: 编辑目标在打开对话框时捕获，避免对话框开启期间切换会话改错会话
    property int editTargetConversationId: 0
    property int editTargetPeerUserId: 0
    // M11A A5: 待滚动到的消息 ID 与所属会话（搜索跳转时设置，消息加载完成后执行滚动）
    property int pendingScrollMessageId: 0
    property int pendingScrollConversationId: 0

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

        // M12.4: 会话视图堆叠容器。每个已打开会话一个 ChatView 实例（由下方
        // chatViewComponent 动态创建），切换会话只改可见性——消息、滚动位置、
        // 输入框草稿、附件下载进度都留在各视图自己的模型里，切走再切回不重建
        Item {
            id: chatStack
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            // 空状态占位：无会话打开时可见（复用 ChatView 的空态视觉）。
            // 声明的视图叠加在其上（子项 z 序在后）
            ChatView {
                id: emptyView
                anchors.fill: parent
                myUserId: mainPage.myUserId
                hasConversation: false
                visible: mainPage.activeViewKey === ""
            }
        }
    }

    // M12.4: 会话视图工厂。信号处理一律按视图自身状态（view.isGroup /
    // view.conversationId / view.peerUserId）分流，而不是读 current* 全局状态：
    // 非活动视图即使有信号回调，也不会把动作落到别的会话上
    Component {
        id: chatViewComponent

        ChatView {
            id: view
            anchors.fill: parent
            myUserId: mainPage.myUserId

            onSendMessage: function(content) {
                if (view.isGroup) {
                    if (view.conversationId > 0)
                        mainPage.sendGroupMessageRequested(view.conversationId, content)
                } else if (view.peerUserId > 0) {
                    mainPage.sendMessageRequested(view.peerUserId, content)
                }
            }
            onGroupInfoRequested: {
                if (view.conversationId > 0)
                    mainPage.getGroupInfoRequested(view.conversationId)
            }
            onEditRequested: function(messageId, content) {
                // 目标会话在打开对话框时捕获：对话框开启期间切换会话不再改错目标
                mainPage.editTargetConversationId = view.conversationId
                mainPage.editTargetPeerUserId = view.isGroup ? 0 : view.peerUserId
                editDialog.openFor(messageId, content)
            }
            onDeleteRequested: function(messageId) {
                confirmDeleteDialog.messageId = messageId
                confirmDeleteDialog.open()
            }
            // P2.2: failed 气泡重发——先移除失败气泡，再按当前会话类型重发
            // （经 MainWindow 走 networkManager.sendMessage，会新建“发送中”气泡）
            onResendRequested: function(clientMessageId, content) {
                view.removeOptimisticMessage(clientMessageId)
                if (view.isGroup) {
                    if (view.conversationId > 0)
                        mainPage.sendGroupMessageRequested(view.conversationId, content)
                } else if (view.peerUserId > 0) {
                    mainPage.sendMessageRequested(view.peerUserId, content)
                }
            }
            onCopyRequested: function(content) {
                mainPage.copyToClipboard(content)
            }
            // P3.2: 转发“正在输入”信号（需已有会话 ID；新会话首条消息前无 ID，跳过）
            onTypingSignal: function(typing) {
                if (view.conversationId > 0)
                    mainPage.typingRequested(view.conversationId, typing)
            }
            onAttachmentSelected: function(fileUrl) {
                if (typeof fileTransfer === "undefined" || !fileTransfer.enabled) {
                    mainPage.fileNotice = "服务端未开启文件传输能力"
                    return
                }
                var convId = view.isGroup ? view.conversationId : 0
                var peerId = view.isGroup ? 0 : view.peerUserId
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
                view.updateFileState(messageId, "downloading")
                view.updateFileProgress(messageId, 0)
                var token = fileTransfer.download(messageId)
                var map = mainPage.downloadTokens
                map[token] = messageId
                mainPage.downloadTokens = map
                if (fileTransfer.isMessageFileAvailable(messageId)) {
                    view.updateFileState(messageId, "available")
                    view.updateFileProgress(messageId, 1.0)
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
            mainPage.editMessageRequested(mainPage.editTargetConversationId,
                                          mainPage.editTargetPeerUserId, messageId, newContent)
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
        messageInfo: mainPage.previewMessageId > 0 ? mainPage.activeMessageInfo(mainPage.previewMessageId) : null
        onSaveRequested: function(msgId) {
            mainPage.pendingSaveMessageId = msgId
            saveFileDialog.messageId = msgId
            saveFileDialog.open()
        }
    }

    MediaPlaybackDialog {
        id: mediaPlaybackDialog
        messageId: mainPage.playMessageId
        messageInfo: mainPage.playMessageId > 0 ? mainPage.activeMessageInfo(mainPage.playMessageId) : null
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
                mainPage.pendingScrollConversationId = conversationId
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
            if (mid !== undefined) mainPage.applyFileProgress(mid, ratio)
        }

        function onTaskFinished(token) {
            var pm = mainPage.taskProgressMap
            delete pm[token]
            mainPage.taskProgressMap = pm
            var mid = mainPage.downloadTokens[token]
            if (mid !== undefined) {
                mainPage.applyFileState(mid, "available")
                mainPage.applyFileProgress(mid, 1.0)
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
                mainPage.applyFileState(mid, "missing")
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
            mainPage.applyFileState(messageId, state)
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

    // M12.4: 视图键。会话有服务端 ID 时用 "c<id>"，新会话首条消息发出前
    //（尚无 ID）用 "p<peerUserId>"，确认后由 adoptPendingView 迁移到正式键
    function viewKeyForConversation(conversationId) { return "c" + conversationId }
    function viewKeyForPeer(peerUserId) { return "p" + peerUserId }
    function viewForConversation(conversationId) { return chatViews[viewKeyForConversation(conversationId)] }
    function activeView() { return chatViews[activeViewKey] }

    function touchView(key) {
        viewUseSeq++
        viewUsage[key] = viewUseSeq
    }

    // 新建视图：从草稿恢复输入文本，随后按需淘汰最久未用的非活动视图
    function createView(key) {
        var view = chatViewComponent.createObject(chatStack)
        chatViews[key] = view
        touchView(key)
        view.inputText = drafts[key] || ""
        evictExcessViews()
        return view
    }

    // 销毁视图并转存其输入框文本（视图不在时草稿只能存在 drafts 表里）
    function destroyView(key) {
        var view = chatViews[key]
        if (!view) return
        if (view.inputText && view.inputText.length > 0) {
            drafts[key] = view.inputText
        } else {
            delete drafts[key]
        }
        // 在途发送记录随视图销毁一并清理：会话键会被后续新建的视图复用，
        // 残留记录会让失败标记落到新视图里毫不相干的气泡上
        for (var cid in pendingSentKeys) {
            if (pendingSentKeys[cid] === key) delete pendingSentKeys[cid]
        }
        delete chatViews[key]
        delete viewUsage[key]
        view.destroy()
    }

    // 超出上限时淘汰最久未使用的非活动视图（活动视图永不淘汰）
    function evictExcessViews() {
        var keys = Object.keys(chatViews)
        while (keys.length > maxCachedViews) {
            var victim = ""
            var oldest = -1
            for (var i = 0; i < keys.length; i++) {
                var k = keys[i]
                if (k === activeViewKey) continue
                var used = viewUsage[k] || 0
                if (oldest < 0 || used < oldest) {
                    oldest = used
                    victim = k
                }
            }
            if (victim.length === 0) break
            destroyView(victim)
            keys = Object.keys(chatViews)
        }
    }

    // 把"待绑定"（尚无服务端 ID）的私聊视图迁到正式会话键上：键迁移 + 补一次
    // 完整加载（视图此前没有服务端历史；按 messageId 合并，乐观气泡不受影响）
    function adoptPendingView(pendingKey, conversationId) {
        var view = chatViews[pendingKey]
        if (!view) return null
        var newKey = viewKeyForConversation(conversationId)
        // 目标键已被另一个视图占用（列表一度缺 peerUserId 等边界下会各自建实例）：
        // 保留已有历史的正式视图，销毁待绑定实例再补收一次。若无条件覆盖，
        // 被覆盖者会脱离 chatViews——既不参与淘汰也不被 resetUi 销毁，其模型里的
        // 已解密消息将驻留到进程退出
        var occupant = chatViews[newKey]
        if (occupant) {
            if (view.inputText && view.inputText.length > 0) drafts[newKey] = view.inputText
            delete drafts[pendingKey]
            delete viewUsage[pendingKey]
            for (var cid in pendingSentKeys) {
                if (pendingSentKeys[cid] === pendingKey) pendingSentKeys[cid] = newKey
            }
            if (activeViewKey === pendingKey) {
                activeViewKey = newKey
                occupant.visible = true
            }
            delete chatViews[pendingKey]
            view.destroy()
            // 待绑定实例里的乐观气泡不在正式视图里，而这条消息此刻已落服务端，
            // 补收一次把它取回来（afterId=0 时走全量加载，同样安全）
            loadMessagesRequested(conversationId, occupant.lastMessageId())
            return occupant
        }
        delete chatViews[pendingKey]
        delete viewUsage[pendingKey]
        chatViews[newKey] = view
        touchView(newKey)
        if (drafts[pendingKey] !== undefined) {
            drafts[newKey] = drafts[pendingKey]
            delete drafts[pendingKey]
        }
        // 在途发送记录的键随之改指，否则确认信号回来时找不到视图
        for (var cid in pendingSentKeys) {
            if (pendingSentKeys[cid] === pendingKey) pendingSentKeys[cid] = newKey
        }
        if (activeViewKey === pendingKey) activeViewKey = newKey
        view.conversationId = conversationId
        loadMessagesRequested(conversationId, 0)
        return view
    }

    // 激活视图：只切可见性并同步 current* 状态，不触碰其它视图的任何状态
    function setActiveView(key, conversationId) {
        activeViewKey = key
        currentConversationId = conversationId
        var keys = Object.keys(chatViews)
        for (var i = 0; i < keys.length; i++) {
            chatViews[keys[i]].visible = (keys[i] === key)
        }
        touchView(key)
        var view = chatViews[key]
        if (!view) return
        // 隐藏期间新消息只改了模型；此前贴底的话重新可见时重新锚定
        if (view.stayAtBottom) view.scrollToBottom()
        // M12.4: 增量补收——离线/隐藏期间错过的消息不会产生 UI 事件
        //（sync_events 回放只落本地缓存）。afterId > 0 不回放缓存页，
        // 视图内容与滚动位置原样保留
        var lastId = view.lastMessageId()
        if (conversationId > 0 && lastId > 0) {
            loadMessagesRequested(conversationId, lastId)
        }
        // 切到已打开会话不重新加载，未读回执在这里补发（服务端据此清角标）
        conversationActivated()
    }

    // 回到"无会话打开"状态（占位空态由此显形）
    function showEmptyState() {
        activeViewKey = ""
        currentConversationId = 0
        currentPeerUserId = 0
        currentPeerUsername = ""
        currentConversationType = "private"
        currentGroupMemberCount = 0
        convList.selectedIndex = -1
        var keys = Object.keys(chatViews)
        for (var i = 0; i < keys.length; i++) {
            chatViews[keys[i]].visible = false
        }
    }

    function openConversation(conv) {
        var convId = conv.conversationId || 0
        if (convId <= 0) return
        var key = viewKeyForConversation(convId)
        var isGroup = (conv.type || "private") === "group"
        var view = chatViews[key]
        if (!view && !isGroup) {
            // 本端刚为该私聊发出首条消息（视图还是"待绑定"键）时，
            // 认领它而不是另起一个实例，两个实例会同时写同一份会话状态
            var pendingKey = viewKeyForPeer(conv.peerUserId || 0)
            if (chatViews[pendingKey]) view = adoptPendingView(pendingKey, convId)
        }
        if (!view) {
            view = createView(key)
            view.conversationId = convId
            view.isGroup = isGroup
            view.peerUserId = isGroup ? 0 : (conv.peerUserId || 0)
            var title = isGroup ? (conv.name || "未命名群组") : (conv.peerUsername || "")
            view.peerUsername = title
            view.chatTitle = title
            view.groupMemberCount = isGroup ? (conv.memberCount || 0) : 0
            view.hasConversation = true
            // P2.3: 注入未读数，供首屏 setMessages 放置未读分隔线
            view.unreadCount = conv.unreadCount || 0
            loadMessagesRequested(convId, 0)
        } else if (view.isGroup) {
            // 复用视图不重载：只把列表带回来的最新成员数补上
            view.groupMemberCount = conv.memberCount || view.groupMemberCount
        }
        currentConversationType = isGroup ? "group" : "private"
        currentPeerUserId = isGroup ? 0 : view.peerUserId
        currentPeerUsername = isGroup ? "" : view.peerUsername
        currentGroupMemberCount = isGroup ? (view.groupMemberCount || 0) : 0
        convList.setSelectedByConversationId(convId)
        setActiveView(key, convId)
    }

    function openChatWithUser(userId, username) {
        var existing = convList.findConversationByPeerId(userId)
        if (existing) {
            openConversation(existing)
            return
        }
        var key = viewKeyForPeer(userId)
        var view = chatViews[key]
        if (!view) {
            view = createView(key)
            view.peerUserId = userId
            view.peerUsername = username
            view.chatTitle = username
            view.hasConversation = true
        }
        currentConversationType = "private"
        currentPeerUserId = userId
        currentPeerUsername = username
        currentGroupMemberCount = 0
        convList.selectedIndex = -1
        setActiveView(key, 0)
    }

    // M12.4: 发送确认（原 bindNewConversation + confirmMessage 合一）。先按幂等键
    // 找回发起视图——用户可能已经切走；新会话首条消息还要把视图迁到正式会话键
    function confirmSentMessage(conversationId, clientMessageId, messageId) {
        var key = pendingSentKeys[clientMessageId] || ""
        delete pendingSentKeys[clientMessageId]
        var view = chatViews[key]
        if (!view) return
        // 迁移前先记下该视图是否正被查看：adoptPendingView 之后 key 已换名
        var wasActive = (activeViewKey === key)
        if (conversationId > 0 && view.conversationId === 0) {
            view = adoptPendingView(key, conversationId)
            if (!view) return
        }
        view.confirmOptimisticMessage(clientMessageId, messageId)
        // 新会话（此前 conversationId 为 0）由此拿到服务端 ID：一次性补进
        // "当前会话"状态与列表高亮，否则已读回执判定与选中态都还停在 0
        if (conversationId > 0 && wasActive && currentConversationId !== conversationId) {
            currentConversationId = conversationId
            convList.setSelectedByConversationId(conversationId)
        }
    }

    function trackOutgoingMessage(clientMessageId, content) {
        var view = activeView()
        if (!view) return
        view.appendOptimisticMessage(clientMessageId, content)
        if (clientMessageId && clientMessageId.length > 0) {
            pendingSentKeys[clientMessageId] = activeViewKey
        }
        if (currentConversationId > 0) {
            var now = new Date()
            var hh = now.getHours()
            var mm = now.getMinutes()
            var timeStr = (hh < 10 ? "0" : "") + hh + ":" + (mm < 10 ? "0" : "") + mm
            convList.updateForNewMessage(currentConversationId, content, timeStr, false)
            // M11A A4: 发送成功后清除该会话的草稿
            delete drafts[activeViewKey]
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
                        var view = viewForConversation(currentConversationId)
                        if (view) {
                            view.chatTitle = conv.name || "未命名群组"
                            view.groupMemberCount = currentGroupMemberCount
                        }
                    }
                    break
                }
            }
        }
    }

    // 消息页按会话路由到对应视图（可能是非活动的隐藏视图）；无视图则丢弃
    function updateMessages(conversationId, messages, hasMore) {
        var view = viewForConversation(conversationId)
        if (!view) return false
        view.setMessages(messages)
        // M12.4: 服务端向前分页没取完（本页满 100 条）时必须接着拉。视图的
        // lastMessageId 会被后续实时消息推高，等下次激活再补收就从更高的 id
        // 起算，中间这段永远取不回来（历史空洞）。续拉的 afterId 取"本页最后
        // 一条"而非视图最大值——实时消息可能已越过本页，用最大值会把还没取到
        // 的那段跳过去
        if (hasMore && messages.length > 0) {
            var pageLastId = messages[messages.length - 1].messageId || 0
            if (pageLastId > 0) loadMessagesRequested(conversationId, pageLastId)
        }
        // M11A A5: 搜索跳转只认目标会话的页，且行已就位才滚动——目标不在
        // 本页（如只在服务端页里）时保留待办，等后续页再试
        if (pendingScrollMessageId > 0 && conversationId == pendingScrollConversationId
                && view.rowForMessageId(pendingScrollMessageId) >= 0) {
            view.scrollToMessage(pendingScrollMessageId)
            pendingScrollMessageId = 0
            pendingScrollConversationId = 0
        }
        return true
    }

    function appendMessage(conversationId, msg) {
        var view = viewForConversation(conversationId)
        if (!view) return false
        view.appendMessage(msg)
        return true
    }

    // 消息 ID 全局唯一：在打开的视图里找持有该行的那个（当前会话或其他
    // 已打开会话都可能是回执的归属）
    function updateMessageStatus(messageId, status) {
        var keys = Object.keys(chatViews)
        for (var i = 0; i < keys.length; i++) {
            var view = chatViews[keys[i]]
            if (view.rowForMessageId(messageId) >= 0) {
                view.updateMessageStatus(messageId, status)
                return
            }
        }
    }

    // P4.1: 传输事件按 messageId 路由到持有该消息的视图
    //（下载期间切走会话后，进度仍要落在原会话的气泡上）
    function applyFileState(messageId, state) {
        var keys = Object.keys(chatViews)
        for (var i = 0; i < keys.length; i++) {
            var view = chatViews[keys[i]]
            if (view.rowForMessageId(messageId) >= 0) {
                view.updateFileState(messageId, state)
                return
            }
        }
    }

    function applyFileProgress(messageId, progress) {
        var keys = Object.keys(chatViews)
        for (var i = 0; i < keys.length; i++) {
            var view = chatViews[keys[i]]
            if (view.rowForMessageId(messageId) >= 0) {
                view.updateFileProgress(messageId, progress)
                return
            }
        }
    }

    // 对话框按 messageId 取消息详情（对话框是模态的，活动视图即发起视图）
    function activeMessageInfo(messageId) {
        var view = activeView()
        return view ? view.getMessageById(messageId) : null
    }

    function updateConversationPreview(message) {
        var now = new Date()
        var hh = now.getHours()
        var mm = now.getMinutes()
        var timeStr = (hh < 10 ? "0" : "") + hh + ":" + (mm < 10 ? "0" : "") + mm
        var contentText = message.contentType === "system"
            ? ChatText.systemMessageText(message.content || "")
            : message.content
        var preview = message.senderUsername ? message.senderUsername + ": " + contentText : contentText
        convList.updateForNewMessage(message.conversationId, preview, timeStr, true)
    }

    function lastIncomingMessageId() {
        var view = activeView()
        return view ? view.lastIncomingMessageId() : 0
    }

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
        var keys = Object.keys(chatViews)
        for (var i = 0; i < keys.length; i++) destroyView(keys[i])
        drafts = ({})
        pendingSentKeys = ({})
        pendingScrollMessageId = 0
        pendingScrollConversationId = 0
        searchMode = "chat"
        showEmptyState()
        convList.reset()
    }

    function openCreatedGroup(conversationId, name) {
        convList.beginLoading()
        loadConversationsRequested()
        var key = viewKeyForConversation(conversationId)
        var view = chatViews[key]
        if (!view) {
            view = createView(key)
            view.conversationId = conversationId
            view.isGroup = true
            view.peerUsername = name
            view.chatTitle = name
            view.hasConversation = true
            loadMessagesRequested(conversationId, 0)
        }
        currentConversationType = "group"
        currentPeerUserId = 0
        currentPeerUsername = ""
        setActiveView(key, conversationId)
    }

    function closeGroupIfCurrent(conversationId) {
        // 退群/被移出：该群视图（可能正被其它视图替代而处于隐藏态）随会话失效
        var key = viewKeyForConversation(conversationId)
        var wasActive = (activeViewKey === key)
        destroyView(key)
        if (wasActive) showEmptyState()
        convList.beginLoading()
        loadConversationsRequested()
    }

    function applyConversationPrefs(conversationId, pinned, muted) { convList.applyPrefs(conversationId, pinned, muted) }

    // P3.1: 会话被删除（本端响应或其他成员/设备推送）——从列表移除，
    // 其视图一并销毁；若为当前打开的会话则回到空态
    function handleConversationDeleted(conversationId) {
        convList.removeConversation(conversationId)
        var key = viewKeyForConversation(conversationId)
        var wasActive = (activeViewKey === key)
        destroyView(key)
        if (wasActive) showEmptyState()
    }

    function applyMessageEdited(conversationId, messageId, content, editedAt) {
        var view = viewForConversation(conversationId)
        if (view) view.updateMessageContent(messageId, content)
    }

    function applyMessageDeleted(conversationId, messageId) {
        var view = viewForConversation(conversationId)
        if (view) view.markMessageDeleted(messageId)
    }

    // P2.2: 复制文本到系统剪贴板（隐藏 TextEdit 选中后 copy）
    function copyToClipboard(text) {
        if (!text || text.length === 0) return
        clipboardHelper.text = text
        clipboardHelper.selectAll()
        clipboardHelper.copy()
        clipboardHelper.deselect()
    }

    function hasPendingSendFrom(key) {
        for (var cid in pendingSentKeys) {
            if (pendingSentKeys[cid] === key) return true
        }
        return false
    }

    // P2.2: 发送失败回调。带幂等键时精确落到发起视图的那条气泡（会话可能已
    // 切走甚至已淘汰）；无键（无效参数/密钥不可用等整批失败）时退回启发式：
    // 优先标活动视图，已切走时若在途发送只来自唯一视图则落到它，多个视图都有
    // 在途发送时不做猜测——宁可漏标，也不能给别的会话的气泡打上失败状态
    function handleSendFailed(clientMessageId) {
        if (clientMessageId && clientMessageId.length > 0) {
            var key = pendingSentKeys[clientMessageId] || ""
            delete pendingSentKeys[clientMessageId]
            if (key.length > 0) {
                var target = chatViews[key]
                // 视图已被淘汰/销毁时这条消息无处可标，不再退回猜测
                if (target) target.markSendingFailed(clientMessageId)
                return
            }
        }
        var view = activeView()
        if (view && view.markSendingFailed("")) return
        // 目标先记键再取对象：qmllint 会把"初始为 null 的变量"定型为 null，
        // 之后再赋视图对象仍报 missing-property
        var targetKey = ""
        var keys = Object.keys(chatViews)
        for (var i = 0; i < keys.length; i++) {
            if (hasPendingSendFrom(keys[i])) {
                if (targetKey.length > 0) return
                targetKey = keys[i]
            }
        }
        if (targetKey.length > 0) chatViews[targetKey].markSendingFailed("")
    }

    // P3.2: 收到对方“正在输入”推送（仅当前会话生效，由 MainWindow 过滤）
    function showPeerTyping(userId, username, typing) {
        var view = activeView()
        if (view) view.setTyping(userId, username, typing)
    }
}
