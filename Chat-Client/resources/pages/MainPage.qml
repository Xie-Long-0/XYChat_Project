import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

import "../theme"
import "../components"

Rectangle {
    id: mainPage
    color: Theme.windowBackground

    signal searchUsersRequested(string query)
    signal addContactRequested(int userId)
    signal loadConversationsRequested()
    signal loadMessagesRequested(int conversationId, int afterId)
    signal sendMessageRequested(int peerUserId, string content)
    signal logoutRequested()
    // M7a: 群组操作信号
    signal loadContactsRequested()
    signal createGroupRequested(string name, var memberIds)
    signal inviteGroupMembersRequested(int conversationId, var userIds)
    signal leaveGroupRequested(int conversationId)
    signal kickGroupMemberRequested(int conversationId, int userId)
    signal getGroupInfoRequested(int conversationId)
    signal sendGroupMessageRequested(int conversationId, string content)
    // M9 特性栈：会话偏好与消息编辑/删除
    signal setConversationPrefsRequested(int conversationId, bool pinned, bool muted)
    signal editMessageRequested(int conversationId, int peerUserId, int messageId, string content)
    signal deleteMessageRequested(int messageId)

    property int myUserId: 0
    property string myUsername: ""

    // 当前选中会话（currentConversationId=0 表示尚未在服务端创建的虚拟会话）
    property int currentConversationId: 0
    property int currentPeerUserId: 0
    property string currentPeerUsername: ""
    // M7a: 当前会话类型（"private"/"group"）
    property string currentConversationType: "private"

    // M8.2: 文件传输的 UI 状态。引擎的进度信号只带任务 token，
    // 而气泡需要 messageId，因此上传用单一在途 token（串行传输），
    // 下载用 token -> messageId 映射
    property string activeUploadToken: ""
    property string activeUploadPhase: ""
    property real activeUploadProgress: 0
    property int pendingSaveMessageId: 0
    property string fileNotice: ""
    property var downloadTokens: ({})
    // M8.3: 当前预览的图片消息（作为 image://xyfile/<id> 的路径段）
    property int previewMessageId: 0
    // M7a: 当前群成员数（群会话头部副标题）
    property int currentGroupMemberCount: 0
    // M7a: 用户搜索用途路由（"chat" 发起对话 / "invite" 群邀请）
    property string searchMode: "chat"

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // 左侧面板
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
                    if (conv) {
                        mainPage.openConversation(conv)
                    }
                }

                onSearchClicked: {
                    searchMode = "chat"
                    searchDialog.open()
                }

                onRefreshClicked: {
                    loadConversationsRequested()
                }

                // M7a: 建群入口
                onCreateGroupClicked: {
                    loadContactsRequested()
                    createGroupDialog.open()
                }

                // M9 特性栈：会话偏好（置顶/免打扰）
                onConversationPrefsRequested: function(conversationId, pinned, muted) {
                    mainPage.setConversationPrefsRequested(conversationId, pinned, muted)
                }
            }

            // M4.5: 侧边栏底部用户信息栏（当前用户 + 登出入口）
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

                    // 当前用户头像
                    Rectangle {
                        width: Theme.avatarSizeSmall
                        height: Theme.avatarSizeSmall
                        radius: Theme.avatarSizeSmall / 2
                        color: Theme.primaryColor

                        Label {
                            anchors.centerIn: parent
                            text: mainPage.myUsername.length > 0 ? mainPage.myUsername[0].toUpperCase() : "?"
                            font.pixelSize: Theme.fontSizeMedium
                            font.weight: Font.Bold
                            color: Theme.textOnPrimary
                        }
                    }

                    // 用户名
                    Label {
                        Layout.fillWidth: true
                        text: mainPage.myUsername
                        font.pixelSize: Theme.fontSizeMedium
                        font.weight: Font.DemiBold
                        color: Theme.textPrimary
                        elide: Text.ElideRight
                    }

                    // 登出按钮
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

                        // 登出图标（门 + 箭头）
                        Canvas {
                            anchors.centerIn: parent
                            width: 16; height: 16
                            onPaint: {
                                var ctx = getContext("2d")
                                ctx.clearRect(0, 0, width, height)
                                ctx.strokeStyle = Theme.textSecondary
                                ctx.lineWidth = 1.5
                                // 门框
                                ctx.beginPath()
                                ctx.moveTo(7, 2)
                                ctx.lineTo(2, 2)
                                ctx.lineTo(2, 14)
                                ctx.lineTo(7, 14)
                                ctx.stroke()
                                // 箭头
                                ctx.beginPath()
                                ctx.moveTo(6, 8)
                                ctx.lineTo(14, 8)
                                ctx.moveTo(11, 5)
                                ctx.lineTo(14, 8)
                                ctx.lineTo(11, 11)
                                ctx.stroke()
                            }
                        }
                    }
                }
            }
        }

        // 右侧聊天区域
        // M8.2: 文件传输横幅（上传进度与取消、一次性提示）
        Rectangle {
            id: transferBanner
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? 46 : 0
            visible: mainPage.activeUploadToken.length > 0 || mainPage.fileNotice.length > 0
            color: Theme.inputBackground
            radius: Theme.radiusMedium

            Behavior on Layout.preferredHeight { NumberAnimation { duration: Theme.animationFast } }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.spacingMedium
                anchors.rightMargin: Theme.spacingMedium
                spacing: Theme.spacingSmall

                Label {
                    Layout.fillWidth: true
                    text: mainPage.activeUploadToken.length > 0
                          ? (mainPage.activeUploadPhase + " "
                             + Math.round(mainPage.activeUploadProgress * 100) + "%")
                          : mainPage.fileNotice
                    elide: Text.ElideMiddle
                    font.pixelSize: Theme.fontSizeSmall
                    color: Theme.textSecondary
                }

                ProgressBar {
                    visible: mainPage.activeUploadToken.length > 0
                    Layout.preferredWidth: 120
                    from: 0; to: 1
                    value: mainPage.activeUploadProgress
                }

                Button {
                    visible: mainPage.activeUploadToken.length > 0
                    text: "取消"
                    onClicked: {
                        fileTransfer.cancelTask(mainPage.activeUploadToken)
                        mainPage.activeUploadToken = ""
                    }
                }

                Button {
                    visible: mainPage.activeUploadToken.length === 0
                              && mainPage.fileNotice.length > 0
                    text: "关闭"
                    onClicked: mainPage.fileNotice = ""
                }
            }
        }

        ChatView {
            id: chatView
            Layout.fillWidth: true
            Layout.fillHeight: true

            myUserId: mainPage.myUserId
            // M7a: 群会话状态绑定
            isGroup: mainPage.currentConversationType === "group"
            groupMemberCount: mainPage.currentGroupMemberCount

            onSendMessage: function(content) {
                // M7a: 群聊与私聊发送分流
                if (mainPage.currentConversationType === "group") {
                    if (mainPage.currentConversationId > 0) {
                        mainPage.sendGroupMessageRequested(mainPage.currentConversationId, content)
                    }
                } else if (mainPage.currentPeerUserId > 0) {
                    mainPage.sendMessageRequested(mainPage.currentPeerUserId, content)
                }
            }

            // M7a: 打开群信息对话框
            onGroupInfoRequested: {
                if (mainPage.currentConversationId > 0) {
                    mainPage.getGroupInfoRequested(mainPage.currentConversationId)
                }
            }

            // M9 特性栈：消息右键菜单（编辑/删除）
            onEditRequested: function(messageId, content) {
                editDialog.openFor(messageId, content)
            }
            onDeleteRequested: function(messageId) {
                confirmDeleteDialog.messageId = messageId
                confirmDeleteDialog.open()
            }

            // M8.2: 附件上传。目标分流与 sendMessage 一致（群聊用 conversationId、
            // 私聊用 peerUserId）；清单与文件密钥全程不经 QML
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
                mainPage.activeUploadPhase = "准备中"
                mainPage.activeUploadProgress = 0
                // 直接把 file URL 交给引擎（内部经 toLocalPath 转本地路径）
                mainPage.activeUploadToken = fileTransfer.uploadAndSend(fileUrl, convId, peerId)
            }

            onFileDownloadRequested: function(messageId) {
                if (typeof fileTransfer === "undefined") {
                    return
                }
                // 先置"下载中"再调用：download() 在缓存命中、清单缺失、未启用
                // 等情况下会在返回前同步 emit 完成/失败信号，而 token 要到返回
                // 后才知道，同步信号因此无法反查到 messageId。调用后再按引擎的
                // 真实状态校准一次，避免气泡永久停在"下载中"
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
                saveFileDialog.open()
            }

            // M8.3: 应用内大图预览
            onFilePreviewRequested: function(messageId) {
                mainPage.previewMessageId = messageId
                imagePreviewDialog.open()
            }
        }
    }

    // M8.3: 应用内大图预览。图像源 image://xyfile/<messageId> 由 C++ 从密文
    // 缓存逐片解密并在内存中解码，明文不落盘（看原图不再必须“另存为”）。
    // M8.3c: 标题带文件名与像素尺寸；footer 提供“另存为”，免得用户为了保存
    // 原图还得先关预览、再回气泡里点“另存为”
    Dialog {
        id: imagePreviewDialog
        modal: true
        anchors.centerIn: parent
        width: Math.min(parent.width * 0.85, 900)
        height: parent.height * 0.85
        // 查不到消息时回退到通用标题（缓存被清或已登出时会走到这条分支）
        title: {
            var info = mainPage.previewMessageId > 0
                       ? chatView.getMessageById(mainPage.previewMessageId) : null
            if (!info || !info.fileName || info.fileName.length === 0) {
                return qsTr("图片预览")
            }
            var suffix = (info.fileWidth > 0 && info.fileHeight > 0)
                         ? "  ·  " + info.fileWidth + "×" + info.fileHeight : ""
            return info.fileName + suffix
        }
        standardButtons: Dialog.NoButton
        onRejected: mainPage.previewMessageId = 0

        contentItem: Item {
            Image {
                id: previewImage
                anchors.fill: parent
                // cache 关掉：每次打开都重新请求，以免缓存被清理或重新下载
                // 后仍展示旧图
                source: mainPage.previewMessageId > 0
                        ? "image://xyfile/" + mainPage.previewMessageId : ""
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                smooth: true
                cache: false
            }

            BusyIndicator {
                anchors.centerIn: parent
                running: previewImage.status === Image.Loading
                visible: running
            }

            Label {
                anchors.centerIn: parent
                width: parent.width * 0.8
                visible: previewImage.status === Image.Error
                         || previewImage.status === Image.Null
                text: qsTr("无法预览：文件未就绪、不是图片，或已超过内存解码上限（64 MB）。可改用“另存为”。")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.Wrap
                horizontalAlignment: Text.AlignHCenter
            }
        }

        footer: DialogButtonBox {
            // 另存为：复用气泡上的保存链路（弹 FileDialog，用户选定路径后
            // C++ 侧流式解密写盘）。预览对话框不关闭，保存完可接着看
            Button {
                text: qsTr("另存为")
                DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
                onClicked: {
                    if (mainPage.previewMessageId > 0) {
                        mainPage.pendingSaveMessageId = mainPage.previewMessageId
                        saveFileDialog.open()
                    }
                }
            }
            Button {
                text: qsTr("关闭")
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            }
        }
    }

    // M8.2: 另存为。明文只写到用户显式选定的路径，缓存目录里
    // 始终只有密文（与 SECURITY.md 的"磁盘无可读明文"口径一致）
    FileDialog {
        id: saveFileDialog
        title: qsTr("保存文件到")
        fileMode: FileDialog.SaveFile
        onAccepted: {
            if (mainPage.pendingSaveMessageId <= 0
                || typeof fileTransfer === "undefined") {
                mainPage.pendingSaveMessageId = 0
                return
            }
            // 转换交给 C++（QUrl::toLocalFile）：QML 的全局 Qt 对象并无
            // urlToLocalFile，而正则剔 file:// 前缀对 UNC 与含 %/#/? 的路径会
            // 静默写到乱码文件名里
            var path = fileTransfer.toLocalPath(selectedFile)
            if (path.length === 0) {
                mainPage.fileNotice = "无法解析所选路径"
                mainPage.pendingSaveMessageId = 0
                return
            }
            if (fileTransfer.saveToFile(mainPage.pendingSaveMessageId, selectedFile)) {
                mainPage.fileNotice = "已保存到 " + path
            } else {
                mainPage.fileNotice = "保存失败：文件未就绪或目标不可写"
            }
            mainPage.pendingSaveMessageId = 0
        }
        onRejected: mainPage.pendingSaveMessageId = 0
    }

    // M8.2: 传输引擎信号接线。进度只带 token，故下载需经映射回到 messageId
    Connections {
        target: typeof fileTransfer !== "undefined" ? fileTransfer : null

        function onTaskProgress(token, phase, done, total) {
            var ratio = total > 0 ? done / total : 0
            if (token === mainPage.activeUploadToken) {
                mainPage.activeUploadPhase = phase
                mainPage.activeUploadProgress = ratio
            }
            var mid = mainPage.downloadTokens[token]
            if (mid !== undefined) {
                chatView.updateFileProgress(mid, ratio)
            }
        }

        function onTaskFinished(token) {
            if (token === mainPage.activeUploadToken) {
                mainPage.activeUploadToken = ""
                mainPage.fileNotice = "文件已发送"
            }
            var mid = mainPage.downloadTokens[token]
            if (mid !== undefined) {
                chatView.updateFileState(mid, "available")
                chatView.updateFileProgress(mid, 1.0)
                delete mainPage.downloadTokens[token]
            }
        }

        function onTaskFailed(token, error) {
            if (token === mainPage.activeUploadToken) {
                mainPage.activeUploadToken = ""
            }
            var mid = mainPage.downloadTokens[token]
            if (mid !== undefined) {
                chatView.updateFileState(mid, "missing")
                delete mainPage.downloadTokens[token]
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

    // 搜索用户对话框
    Dialog {
        id: searchDialog
        title: "搜索用户"
        modal: true
        anchors.centerIn: parent
        width: 360
        padding: Theme.spacingLarge

        background: Rectangle {
            radius: Theme.radiusLarge
            color: Theme.windowBackground
            border.width: 1
            border.color: Theme.borderColor
        }

        contentItem: ColumnLayout {
            spacing: Theme.spacingMedium

            Label {
                text: "输入用户名搜索"
                font.pixelSize: Theme.fontSizeMedium
                color: Theme.textPrimary
            }

            TextField {
                id: searchField
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.inputHeight
                placeholderText: "用户名..."
                font.pixelSize: Theme.fontSizeMedium
                color: Theme.textPrimary
                placeholderTextColor: Theme.inputPlaceholderColor
                selectByMouse: true
                background: Rectangle {
                    radius: Theme.radiusSmall
                    color: Theme.inputBackground
                    border.width: searchField.activeFocus ? 2 : 1
                    border.color: searchField.activeFocus ? Theme.inputFocusBorderColor : Theme.inputBorderColor
                }
                onAccepted: searchDialog.doSearch()
            }

            ListView {
                id: searchResults
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(contentHeight, 200)
                clip: true
                visible: searchResultModel.count > 0
                model: ListModel { id: searchResultModel }

                delegate: Rectangle {
                    width: searchResults.width
                    height: 44
                    color: searchDelegateMouse.containsMouse ? Theme.hoverColor : "transparent"
                    radius: Theme.radiusSmall

                    MouseArea {
                        id: searchDelegateMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            // M4.5: 点击搜索结果直接发起对话，同时后台添加联系人
                            mainPage.openChatWithUser(model.userId, model.username)
                            addContactRequested(model.userId)
                            searchDialog.close()
                        }
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.spacingMedium
                        anchors.rightMargin: Theme.spacingMedium

                        Rectangle {
                            width: 32; height: 32; radius: 16
                            color: Theme.primaryColor
                            Label {
                                anchors.centerIn: parent
                                text: model.username.length > 0 ? model.username[0].toUpperCase() : "?"
                                font.pixelSize: Theme.fontSizeSmall
                                font.weight: Font.Bold
                                color: Theme.textOnPrimary
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            text: model.username + " (ID: " + model.userId + ")"
                            font.pixelSize: Theme.fontSizeMedium
                            color: Theme.textPrimary
                        }
                    }
                }
            }

            Label {
                id: searchStatus
                text: ""
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.textSecondary
                visible: text !== ""
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacingSmall

                Item { Layout.fillWidth: true }

                Button {
                    text: "取消"
                    flat: true
                    font.pixelSize: Theme.fontSizeMedium
                    contentItem: Label {
                        text: parent.text
                        font: parent.font
                        color: Theme.textSecondary
                    }
                    background: null
                    onClicked: searchDialog.close()
                }

                Button {
                    text: "搜索"
                    font.pixelSize: Theme.fontSizeMedium
                    font.weight: Font.DemiBold
                    background: Rectangle {
                        radius: Theme.radiusSmall
                        color: searchDialogBtn.pressed ? Theme.loginButtonPressed
                             : (searchDialogBtn.hovered ? Theme.loginButtonHover : Theme.primaryColor)
                    }
                    contentItem: Label {
                        text: parent.text
                        font: parent.font
                        color: Theme.textOnPrimary
                        horizontalAlignment: Text.AlignHCenter
                    }
                    id: searchDialogBtn
                    onClicked: searchDialog.doSearch()
                }
            }
        }

        function doSearch() {
            var query = searchField.text.trim()
            if (query.length > 0) {
                searchStatus.text = "搜索中..."
                searchUsersRequested(query)
            }
        }

        function showResults(users) {
            searchResultModel.clear()
            if (users.length === 0) {
                searchStatus.text = "未找到匹配的用户"
                return
            }
            searchStatus.text = ""
            for (var i = 0; i < users.length; i++) {
                var u = users[i]
                searchResultModel.append({
                    userId: u.userId || 0,
                    username: u.username || ""
                })
            }
        }

        onClosed: {
            searchField.text = ""
            searchResultModel.clear()
            searchStatus.text = ""
        }
    }

    // M7a: 建群对话框（群名 + 联系人多选）
    Dialog {
        id: createGroupDialog
        title: "新建群组"
        modal: true
        anchors.centerIn: parent
        width: 380
        padding: Theme.spacingLarge

        background: Rectangle {
            radius: Theme.radiusLarge
            color: Theme.windowBackground
            border.width: 1
            border.color: Theme.borderColor
        }

        contentItem: ColumnLayout {
            spacing: Theme.spacingMedium

            TextField {
                id: groupNameField
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.inputHeight
                placeholderText: "群名称（1-64 字符）"
                font.pixelSize: Theme.fontSizeMedium
                color: Theme.textPrimary
                placeholderTextColor: Theme.inputPlaceholderColor
                selectByMouse: true
                background: Rectangle {
                    radius: Theme.radiusSmall
                    color: Theme.inputBackground
                    border.width: groupNameField.activeFocus ? 2 : 1
                    border.color: groupNameField.activeFocus ? Theme.inputFocusBorderColor : Theme.inputBorderColor
                }
            }

            Label {
                text: "选择初始成员（联系人）"
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.textSecondary
                visible: contactModel.count > 0
            }

            ListView {
                id: contactList
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(contentHeight, 220)
                clip: true
                visible: contactModel.count > 0
                model: ListModel { id: contactModel }

                delegate: Rectangle {
                    width: contactList.width
                    height: 40
                    color: contactDelegateMouse.containsMouse ? Theme.hoverColor : "transparent"
                    radius: Theme.radiusSmall

                    MouseArea {
                        id: contactDelegateMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: contactModel.setProperty(index, "selected", !model.selected)
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.spacingMedium
                        anchors.rightMargin: Theme.spacingMedium

                        Label {
                            text: model.selected ? "☑" : "☐"
                            font.pixelSize: Theme.fontSizeMedium
                            color: model.selected ? Theme.primaryColor : Theme.textTertiary
                        }

                        Label {
                            Layout.fillWidth: true
                            text: model.username + " (ID: " + model.userId + ")"
                            font.pixelSize: Theme.fontSizeMedium
                            color: Theme.textPrimary
                            elide: Text.ElideRight
                        }
                    }
                }
            }

            Label {
                text: "暂无联系人，可先搜索添加联系人"
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.textTertiary
                visible: contactModel.count === 0
            }

            Label {
                id: createGroupStatus
                text: ""
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.textSecondary
                visible: text !== ""
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacingSmall

                Item { Layout.fillWidth: true }

                Button {
                    text: "取消"
                    flat: true
                    font.pixelSize: Theme.fontSizeMedium
                    contentItem: Label {
                        text: parent.text
                        font: parent.font
                        color: Theme.textSecondary
                    }
                    background: null
                    onClicked: createGroupDialog.close()
                }

                Button {
                    id: createGroupBtn
                    text: "创建"
                    font.pixelSize: Theme.fontSizeMedium
                    font.weight: Font.DemiBold
                    background: Rectangle {
                        radius: Theme.radiusSmall
                        color: createGroupBtn.pressed ? Theme.loginButtonPressed
                             : (createGroupBtn.hovered ? Theme.loginButtonHover : Theme.primaryColor)
                    }
                    contentItem: Label {
                        text: parent.text
                        font: parent.font
                        color: Theme.textOnPrimary
                        horizontalAlignment: Text.AlignHCenter
                    }
                    onClicked: createGroupDialog.doCreate()
                }
            }
        }

        function showContacts(contacts) {
            contactModel.clear()
            for (var i = 0; i < contacts.length; i++) {
                contactModel.append({
                    userId: contacts[i].userId || 0,
                    username: contacts[i].username || "",
                    selected: false
                })
            }
        }

        function doCreate() {
            var name = groupNameField.text.trim()
            if (name.length === 0 || name.length > 64) {
                createGroupStatus.text = "群名称需为 1-64 字符"
                return
            }
            var ids = []
            for (var i = 0; i < contactModel.count; i++) {
                var c = contactModel.get(i)
                if (c.selected) {
                    ids.push(c.userId)
                }
            }
            createGroupRequested(name, ids)
            close()
        }

        onClosed: {
            groupNameField.text = ""
            contactModel.clear()
            createGroupStatus.text = ""
        }
    }

    // M7a: 群信息对话框（成员列表/邀请/踢人/退群）
    Dialog {
        id: groupInfoDialog
        title: "群信息"
        modal: true
        anchors.centerIn: parent
        width: 400
        padding: Theme.spacingLarge

        property int convId: 0
        property string myRole: "member"

        background: Rectangle {
            radius: Theme.radiusLarge
            color: Theme.windowBackground
            border.width: 1
            border.color: Theme.borderColor
        }

        contentItem: ColumnLayout {
            spacing: Theme.spacingMedium

            Label {
                id: groupInfoTitleText
                font.pixelSize: Theme.fontSizeLarge
                font.weight: Font.DemiBold
                color: Theme.textPrimary
                elide: Text.ElideRight
                Layout.fillWidth: true
            }

            Label {
                id: groupInfoSubtitle
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.textSecondary
            }

            ListView {
                id: memberList
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(contentHeight, 260)
                clip: true
                model: ListModel { id: memberModel }

                delegate: Rectangle {
                    width: memberList.width
                    height: 44
                    color: memberDelegateMouse.containsMouse ? Theme.hoverColor : "transparent"
                    radius: Theme.radiusSmall

                    MouseArea {
                        id: memberDelegateMouse
                        anchors.fill: parent
                        hoverEnabled: true
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.spacingMedium
                        anchors.rightMargin: Theme.spacingMedium

                        Rectangle {
                            width: 28; height: 28; radius: 14
                            color: Theme.primaryColor
                            Label {
                                anchors.centerIn: parent
                                text: model.username.length > 0 ? model.username[0].toUpperCase() : "?"
                                font.pixelSize: Theme.fontSizeSmall
                                font.weight: Font.Bold
                                color: Theme.textOnPrimary
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            // 使用 == 兼容 C++ qint64 经 JSON 传递到 QML 后可能为 string/number 的情况
                            text: model.username + (model.userId == mainPage.myUserId ? "（我）" : "")
                            font.pixelSize: Theme.fontSizeMedium
                            color: Theme.textPrimary
                            elide: Text.ElideRight
                        }

                        Label {
                            text: groupInfoDialog.roleDisplay(model.role)
                            font.pixelSize: Theme.fontSizeSmall
                            color: model.role === "owner" ? Theme.primaryColor : Theme.textTertiary
                        }

                        // 踢人按钮（层级保护：owner 可移除 admin/member，admin 仅可移除 member）
                        Button {
                            visible: groupInfoDialog.canKick(model.role)
                                     && model.userId !== mainPage.myUserId
                            text: "移除"
                            flat: true
                            font.pixelSize: Theme.fontSizeSmall
                            contentItem: Label {
                                text: parent.text
                                font: parent.font
                                color: Theme.unreadBadgeColor
                            }
                            background: null
                            onClicked: {
                                kickGroupMemberRequested(groupInfoDialog.convId, model.userId)
                                // 刷新群信息（服务端推送 group_changed 亦会刷新会话列表）
                                getGroupInfoRequested(groupInfoDialog.convId)
                            }
                        }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacingSmall

                Button {
                    text: "邀请成员"
                    flat: true
                    font.pixelSize: Theme.fontSizeMedium
                    contentItem: Label {
                        text: parent.text
                        font: parent.font
                        color: Theme.primaryColor
                    }
                    background: null
                    onClicked: inviteDialog.open()
                }

                Item { Layout.fillWidth: true }

                Button {
                    text: "退出群聊"
                    flat: true
                    font.pixelSize: Theme.fontSizeMedium
                    contentItem: Label {
                        text: parent.text
                        font: parent.font
                        color: Theme.unreadBadgeColor
                    }
                    background: null
                    onClicked: {
                        leaveGroupRequested(groupInfoDialog.convId)
                        groupInfoDialog.close()
                    }
                }
            }
        }

        function showInfo(info) {
            convId = info.conversationId || 0
            myRole = info.myRole || "member"
            groupInfoTitleText.text = info.name || "未命名群组"
            groupInfoSubtitle.text = (info.memberCount || 0) + " 位成员 · 你是" + roleDisplay(myRole)
            memberModel.clear()
            var members = info.members || []
            for (var i = 0; i < members.length; i++) {
                memberModel.append({
                    userId: members[i].userId || 0,
                    username: members[i].username || "",
                    role: members[i].role || "member"
                })
            }
            if (!visible) {
                open()
            }
        }

        function roleDisplay(role) {
            if (role === "owner") return "群主"
            if (role === "admin") return "管理员"
            return "成员"
        }

        function canKick(memberRole) {
            if (myRole === "owner") return memberRole !== "owner"
            if (myRole === "admin") return memberRole === "member"
            return false
        }
    }

    // M7a: 邀请成员对话框（搜索用户后多选邀请）
    Dialog {
        id: inviteDialog
        title: "邀请成员"
        modal: true
        anchors.centerIn: parent
        width: 360
        padding: Theme.spacingLarge

        background: Rectangle {
            radius: Theme.radiusLarge
            color: Theme.windowBackground
            border.width: 1
            border.color: Theme.borderColor
        }

        contentItem: ColumnLayout {
            spacing: Theme.spacingMedium

            TextField {
                id: inviteSearchField
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.inputHeight
                placeholderText: "搜索用户名..."
                font.pixelSize: Theme.fontSizeMedium
                color: Theme.textPrimary
                placeholderTextColor: Theme.inputPlaceholderColor
                selectByMouse: true
                background: Rectangle {
                    radius: Theme.radiusSmall
                    color: Theme.inputBackground
                    border.width: inviteSearchField.activeFocus ? 2 : 1
                    border.color: inviteSearchField.activeFocus ? Theme.inputFocusBorderColor : Theme.inputBorderColor
                }
                onAccepted: inviteDialog.doSearch()
            }

            ListView {
                id: inviteResultList
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(contentHeight, 200)
                clip: true
                visible: inviteResultModel.count > 0
                model: ListModel { id: inviteResultModel }

                delegate: Rectangle {
                    width: inviteResultList.width
                    height: 40
                    color: inviteDelegateMouse.containsMouse ? Theme.hoverColor : "transparent"
                    radius: Theme.radiusSmall

                    MouseArea {
                        id: inviteDelegateMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: inviteResultModel.setProperty(index, "selected", !model.selected)
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.spacingMedium
                        anchors.rightMargin: Theme.spacingMedium

                        Label {
                            text: model.selected ? "☑" : "☐"
                            font.pixelSize: Theme.fontSizeMedium
                            color: model.selected ? Theme.primaryColor : Theme.textTertiary
                        }

                        Label {
                            Layout.fillWidth: true
                            text: model.username + " (ID: " + model.userId + ")"
                            font.pixelSize: Theme.fontSizeMedium
                            color: Theme.textPrimary
                            elide: Text.ElideRight
                        }
                    }
                }
            }

            Label {
                id: inviteStatus
                text: ""
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.textSecondary
                visible: text !== ""
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacingSmall

                Item { Layout.fillWidth: true }

                Button {
                    text: "取消"
                    flat: true
                    font.pixelSize: Theme.fontSizeMedium
                    contentItem: Label {
                        text: parent.text
                        font: parent.font
                        color: Theme.textSecondary
                    }
                    background: null
                    onClicked: inviteDialog.close()
                }

                Button {
                    id: inviteConfirmBtn
                    text: "邀请"
                    font.pixelSize: Theme.fontSizeMedium
                    font.weight: Font.DemiBold
                    background: Rectangle {
                        radius: Theme.radiusSmall
                        color: inviteConfirmBtn.pressed ? Theme.loginButtonPressed
                             : (inviteConfirmBtn.hovered ? Theme.loginButtonHover : Theme.primaryColor)
                    }
                    contentItem: Label {
                        text: parent.text
                        font: parent.font
                        color: Theme.textOnPrimary
                        horizontalAlignment: Text.AlignHCenter
                    }
                    onClicked: inviteDialog.doInvite()
                }
            }
        }

        function doSearch() {
            var query = inviteSearchField.text.trim()
            if (query.length > 0) {
                inviteStatus.text = "搜索中..."
                searchMode = "invite"
                searchUsersRequested(query)
            }
        }

        function showResults(users) {
            inviteResultModel.clear()
            if (users.length === 0) {
                inviteStatus.text = "未找到匹配的用户"
                return
            }
            inviteStatus.text = ""
            for (var i = 0; i < users.length; i++) {
                inviteResultModel.append({
                    userId: users[i].userId || 0,
                    username: users[i].username || "",
                    selected: false
                })
            }
        }

        function doInvite() {
            var ids = []
            for (var i = 0; i < inviteResultModel.count; i++) {
                var u = inviteResultModel.get(i)
                if (u.selected) {
                    ids.push(u.userId)
                }
            }
            if (ids.length === 0) {
                inviteStatus.text = "请先选择要邀请的用户"
                return
            }
            inviteGroupMembersRequested(mainPage.currentConversationId, ids)
            close()
        }

        onClosed: {
            inviteSearchField.text = ""
            inviteResultModel.clear()
            inviteStatus.text = ""
            searchMode = "chat"
        }
    }

    // M9 特性栈：编辑消息对话框（预填原文，保存后提交重新加密）
    Dialog {
        id: editDialog
        title: "编辑消息"
        modal: true
        anchors.centerIn: parent
        width: 400
        padding: Theme.spacingLarge

        property int messageId: 0

        background: Rectangle {
            radius: Theme.radiusLarge
            color: Theme.windowBackground
            border.width: 1
            border.color: Theme.borderColor
        }

        contentItem: ColumnLayout {
            spacing: Theme.spacingMedium

            TextField {
                id: editField
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.inputHeight
                placeholderText: "输入新内容..."
                font.pixelSize: Theme.fontSizeMedium
                color: Theme.textPrimary
                placeholderTextColor: Theme.inputPlaceholderColor
                selectByMouse: true
                background: Rectangle {
                    radius: Theme.radiusSmall
                    color: Theme.inputBackground
                    border.width: editField.activeFocus ? 2 : 1
                    border.color: editField.activeFocus ? Theme.inputFocusBorderColor : Theme.inputBorderColor
                }
            }

            Label {
                id: editStatus
                text: ""
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.textSecondary
                visible: text !== ""
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacingSmall

                Item { Layout.fillWidth: true }

                Button {
                    text: "取消"
                    flat: true
                    font.pixelSize: Theme.fontSizeMedium
                    contentItem: Label {
                        text: parent.text
                        font: parent.font
                        color: Theme.textSecondary
                    }
                    background: null
                    onClicked: editDialog.close()
                }

                Button {
                    id: editConfirmBtn
                    text: "保存"
                    font.pixelSize: Theme.fontSizeMedium
                    font.weight: Font.DemiBold
                    background: Rectangle {
                        radius: Theme.radiusSmall
                        color: editConfirmBtn.pressed ? Theme.loginButtonPressed
                             : (editConfirmBtn.hovered ? Theme.loginButtonHover : Theme.primaryColor)
                    }
                    contentItem: Label {
                        text: parent.text
                        font: parent.font
                        color: Theme.textOnPrimary
                        horizontalAlignment: Text.AlignHCenter
                    }
                    onClicked: editDialog.doEdit()
                }
            }
        }

        function openFor(messageId, content) {
            editDialog.messageId = messageId
            editField.text = content
            editStatus.text = ""
            open()
        }

        function doEdit() {
            var text = editField.text.trim()
            if (text.length === 0) {
                editStatus.text = "内容不能为空"
                return
            }
            if (editDialog.messageId <= 0) {
                editStatus.text = "消息 ID 无效"
                return
            }
            // 私聊 peerUserId 为当前会话对方；群聊为 0（走 Sender-Key 重加密）
            var peer = mainPage.currentConversationType === "group"
                ? 0 : mainPage.currentPeerUserId
            mainPage.editMessageRequested(mainPage.currentConversationId, peer,
                                          editDialog.messageId, text)
            close()
        }

        onClosed: {
            editField.text = ""
            editDialog.messageId = 0
            editStatus.text = ""
        }
    }

    // M9 特性栈：删除确认对话框
    Dialog {
        id: confirmDeleteDialog
        title: "删除消息"
        modal: true
        anchors.centerIn: parent
        width: 320
        padding: Theme.spacingLarge

        property int messageId: 0

        background: Rectangle {
            radius: Theme.radiusLarge
            color: Theme.windowBackground
            border.width: 1
            border.color: Theme.borderColor
        }

        contentItem: ColumnLayout {
            spacing: Theme.spacingLarge

            Label {
                Layout.fillWidth: true
                text: "删除后所有会话成员都将看到“消息已删除”，此操作不可撤销。是否继续？"
                font.pixelSize: Theme.fontSizeMedium
                color: Theme.textPrimary
                wrapMode: Text.Wrap
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacingSmall

                Item { Layout.fillWidth: true }

                Button {
                    text: "取消"
                    flat: true
                    font.pixelSize: Theme.fontSizeMedium
                    contentItem: Label {
                        text: parent.text
                        font: parent.font
                        color: Theme.textSecondary
                    }
                    background: null
                    onClicked: confirmDeleteDialog.close()
                }

                Button {
                    id: deleteConfirmBtn
                    text: "删除"
                    font.pixelSize: Theme.fontSizeMedium
                    font.weight: Font.DemiBold
                    background: Rectangle {
                        radius: Theme.radiusSmall
                        color: deleteConfirmBtn.pressed ? Theme.loginErrorColor
                             : (deleteConfirmBtn.hovered ? Theme.unreadBadgeMutedColor : Theme.unreadBadgeColor)
                    }
                    contentItem: Label {
                        text: parent.text
                        font: parent.font
                        color: Theme.textOnPrimary
                        horizontalAlignment: Text.AlignHCenter
                    }
                    onClicked: {
                        mainPage.deleteMessageRequested(confirmDeleteDialog.messageId)
                        confirmDeleteDialog.close()
                    }
                }
            }
        }

        onClosed: {
            confirmDeleteDialog.messageId = 0
        }
    }

    // ── 会话操作 ──
    // 打开一个既有会话
    function openConversation(conv) {
        currentConversationId = conv.conversationId
        currentConversationType = conv.type || "private"

        if (currentConversationType === "group") {
            // M7a: 群会话：标题为群名，私聊字段不适用
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

        convList.setSelectedByConversationId(conv.conversationId)
        loadMessagesRequested(conv.conversationId, 0)
    }

    // M4.5: 与指定用户开始聊天；无既有会话时建立虚拟会话（conversationId=0），
    // 首条消息发送成功后由 bindNewConversation() 绑定服务端会话 ID
    function openChatWithUser(userId, username) {
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

        convList.selectedIndex = -1
    }

    // M4.5: 首条消息发送成功后绑定服务端返回的会话 ID
    function bindNewConversation(conversationId) {
        // 使用 == 兼容 C++ qint64 经 JSON 传递到 QML 后可能为 string/number 的情况
        if (currentConversationId == 0 && conversationId > 0) {
            currentConversationId = conversationId
            convList.setSelectedByConversationId(conversationId)
        }
    }

    // M4.5: 乐观插入“发送中”消息（由 MainWindow 在调用 networkManager.sendMessage 后回调）
    function trackOutgoingMessage(clientMessageId, content) {
        chatView.appendOptimisticMessage(clientMessageId, content)
        if (currentConversationId > 0) {
            var now = new Date()
            var hh = now.getHours()
            var mm = now.getMinutes()
            var timeStr = (hh < 10 ? "0" : "") + hh + ":" + (mm < 10 ? "0" : "") + mm
            convList.updateForNewMessage(currentConversationId, content, timeStr, false)
        }
    }

    // 公共方法
    function updateConversations(conversations) {
        convList.updateConversations(conversations)
        // 会话刷新后恢复当前选中高亮
        if (currentConversationId > 0) {
            convList.setSelectedByConversationId(currentConversationId)
            // M7a: 当前群会话的成员数/群名变化同步到聊天区
            for (var i = 0; i < conversations.length; i++) {
                var conv = conversations[i]
                // 使用 == 兼容 C++ qint64 经 JSON 传递到 QML 后可能为 string/number 的情况
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
    }

    function appendMessage(msg) {
        chatView.appendMessage(msg)
    }

    // M4.5: 服务端确认后更新乐观消息
    function confirmMessage(clientMessageId, messageId) {
        chatView.confirmOptimisticMessage(clientMessageId, messageId)
    }

    // M4.5: 消息状态推送（已送达/已读）
    function updateMessageStatus(messageId, status) {
        chatView.updateMessageStatus(messageId, status)
    }

    // M4.5: 新消息到达但当前未打开该会话时，本地更新预览与未读角标
    function updateConversationPreview(message) {
        var now = new Date()
        var hh = now.getHours()
        var mm = now.getMinutes()
        var timeStr = (hh < 10 ? "0" : "") + hh + ":" + (mm < 10 ? "0" : "") + mm
        // M7a: 群系统消息预览用可读摘要
        var contentText = message.contentType === "system"
            ? chatView.systemMessageText(message.content || "")
            : message.content
        var preview = message.senderUsername ? message.senderUsername + ": " + contentText
                                             : contentText
        convList.updateForNewMessage(message.conversationId, preview, timeStr, true)
    }

    // M4.5: 当前会话中最后一条对方消息 ID（用于发送已读回执）
    function lastIncomingMessageId() {
        return chatView.lastIncomingMessageId()
    }

    function showSearchResults(users) {
        // M7a: 搜索结果按用途路由（发起对话 / 群邀请）
        if (searchMode === "invite") {
            inviteDialog.showResults(users)
        } else {
            searchDialog.showResults(users)
        }
    }

    // M7a: 联系人列表（建群对话框成员选择）
    function showContacts(contacts) {
        createGroupDialog.showContacts(contacts)
    }

    // M7a: 群信息响应（打开/刷新群信息对话框）
    function showGroupInfo(info) {
        groupInfoDialog.showInfo(info)
    }

    // M4.5: 登出时重置界面状态
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

    // M7a: 建群成功后打开新群会话
    function openCreatedGroup(conversationId, name) {
        loadConversationsRequested()
        currentConversationId = conversationId
        currentConversationType = "group"
        currentPeerUserId = 0
        currentPeerUsername = ""
        chatView.peerUsername = name
        chatView.chatTitle = name
        chatView.hasConversation = true
        chatView.clearMessages()
        loadMessagesRequested(conversationId, 0)
    }

    // M7a: 退群/被移出后若当前正在该群，关闭聊天区
    function closeGroupIfCurrent(conversationId) {
        // 使用 == 兼容 C++ qint64 经 JSON 传递到 QML 后可能为 string/number 的情况
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
        loadConversationsRequested()
    }

    // M9 特性栈：会话偏好推送回填（服务端 conversation_prefs 通知）
    function applyConversationPrefs(conversationId, pinned, muted) {
        convList.applyPrefs(conversationId, pinned, muted)
    }

    // M9 特性栈：消息编辑结果回填（本端响应或其他成员推送）
    function applyMessageEdited(conversationId, messageId, content, editedAt) {
        // 使用 == 兼容 C++ qint64 经 JSON 传递到 QML 后可能为 string/number 的情况
        if (conversationId == currentConversationId) {
            chatView.updateMessageContent(messageId, content)
        }
    }

    // M9 特性栈：消息删除结果回填（本端响应或其他成员推送）
    function applyMessageDeleted(conversationId, messageId) {
        // 使用 == 兼容 C++ qint64 经 JSON 传递到 QML 后可能为 string/number 的情况
        if (conversationId == currentConversationId) {
            chatView.markMessageDeleted(messageId)
        }
    }
}
