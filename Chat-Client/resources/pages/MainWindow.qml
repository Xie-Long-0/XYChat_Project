import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtQuick.Layouts
import QWindowKit

import "../theme"
import "../components"

Window {
    id: mainWindow
    // M4.5 修复：作为独立根窗口由 main.cpp 加载，供其按 objectName 查找并注入登录窗口
    objectName: "mainWindow"
    width: 1000
    height: 650
    minimumWidth: 700
    minimumHeight: 450
    visible: false
    color: "transparent"
    title: "XYChat"

    // 登出信号（由登录窗口处理窗口切换）
    signal logoutRequested()

    property int myUserId: 0
    property string myUsername: ""

    // QWindowKit WindowAgent
    WindowAgent {
        id: windowAgent
    }

    Component.onCompleted: {
        windowAgent.setup(mainWindow)
    }

    // M11A: 关闭主窗口——若设置启用“最小化到托盘”且托盘可用则隐藏窗口而非退出应用
    onClosing: function(close) {
        if (typeof appSettings !== "undefined" && appSettings.minimizeToTray
            && typeof trayManager !== "undefined" && trayManager.available) {
            trayManager.minimizeToTray()
            close.accepted = false
        } else {
            Qt.quit()
        }
    }

    // 主布局
    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // 自定义标题栏
        TitleBar {
            id: titleBar
            Layout.fillWidth: true
            window: mainWindow
            windowAgent: windowAgent
            title: "XYChat"
        }

        // M10: 网络状态条（断线/重连/离线时可见，Authenticated 时隐藏）
        NetworkStatusBar {
            Layout.fillWidth: true
            networkState: networkManager.state
        }

        // 主页面
        MainPage {
            id: mainPage
            Layout.fillWidth: true
            Layout.fillHeight: true

            myUserId: mainWindow.myUserId
            myUsername: mainWindow.myUsername

            onSearchUsersRequested: function(query) {
                networkManager.searchUsers(query)
            }

            onAddContactRequested: function(userId) {
                networkManager.addContact(userId)
            }

            onLoadConversationsRequested: {
                networkManager.getConversations()
            }

            onLoadMessagesRequested: function(conversationId, afterId) {
                networkManager.syncMessages(conversationId, afterId)
            }

            onSendMessageRequested: function(peerUserId, content) {
                // M4.5: 发送后以返回的幂等键跟踪乐观消息气泡
                var clientMessageId = networkManager.sendMessage(peerUserId, content)
                mainPage.trackOutgoingMessage(clientMessageId, content)
            }

            onLogoutRequested: {
                mainPage.resetUi()
                mainWindow.logoutRequested()
            }

            // M7a: 群组操作信号接入
            onLoadContactsRequested: {
                networkManager.getContacts()
            }

            onCreateGroupRequested: function(name, memberIds) {
                networkManager.createGroup(name, memberIds)
            }

            onInviteGroupMembersRequested: function(conversationId, userIds) {
                networkManager.inviteGroupMembers(conversationId, userIds)
            }

            onLeaveGroupRequested: function(conversationId) {
                networkManager.leaveGroup(conversationId)
            }

            onKickGroupMemberRequested: function(conversationId, userId) {
                networkManager.kickGroupMember(conversationId, userId)
            }

            onGetGroupInfoRequested: function(conversationId) {
                networkManager.getGroupInfo(conversationId)
            }

            onSendGroupMessageRequested: function(conversationId, content) {
                // 群消息同样以幂等键跟踪乐观气泡
                var clientMessageId = networkManager.sendGroupMessage(conversationId, content)
                if (clientMessageId) {
                    mainPage.trackOutgoingMessage(clientMessageId, content)
                }
            }

            // M9 特性栈：会话偏好与消息编辑/删除
            onSetConversationPrefsRequested: function(conversationId, pinned, muted) {
                networkManager.setConversationPrefs(conversationId, pinned, muted)
            }

            onEditMessageRequested: function(conversationId, peerUserId, messageId, content) {
                networkManager.editMessage(conversationId, peerUserId, messageId, content)
            }

            onDeleteMessageRequested: function(messageId) {
                networkManager.deleteMessage(messageId)
            }

            onMarkConversationReadRequested: function(conversationId) {
                networkManager.getConversations()
            }

            // P3.2: “正在输入”信号（C++ 侧节流，即发即忘）
            onTypingRequested: function(conversationId, typing) {
                networkManager.sendTyping(conversationId, typing)
            }

            // M12.4: 会话被激活（含"切到已打开会话"这条不再重新加载的路径）。
            // 已读回执原先挂在"当前会话的消息页到达"上，切回旧会话时没有页
            // 到达，角标会一直留着，故在激活点补发
            onConversationActivated: {
                sendReadAck()
            }

            // P3.1: 会话整表删除
            onDeleteConversationRequested: function(conversationId) {
                networkManager.deleteConversation(conversationId)
            }
        }
    }

    // NetworkManager 聊天信号连接
    Connections {
        target: networkManager

        function onConversationsResult(conversations) {
            // 将 QJsonArray 转为 JS 数组
            var convs = []
            for (var i = 0; i < conversations.length; i++) {
                convs.push(conversations[i])
            }
            mainPage.updateConversations(convs)
        }

        function onMessagesSynced(conversationId, messages, hasMore) {
            var msgs = []
            for (var i = 0; i < messages.length; i++) {
                msgs.push(messages[i])
            }
            // M12.4: 页按会话路由到对应视图（可能是切走后仍保留的后台视图）；
            // 视图已不存在（被淘汰/未打开过）时该页无处可放，直接丢弃——
            // 视图重建时 afterId=0 会重新拉取
            if (mainPage.updateMessages(conversationId, msgs, hasMore)) {
                // M4.5: 打开会话时对最后一条对方消息发送已读回执
                // 使用 == 兼容 C++ qint64 经 JSON 传递到 QML 后可能为 string/number 的情况
                if (conversationId == mainPage.currentConversationId) {
                    sendReadAck()
                }
            }
        }

        function onNewMessageReceived(message) {
            var convId = message.conversationId
            // 使用 == 兼容 C++ qint64 经 JSON 传递到 QML 后可能为 string/number 的情况
            var active = (convId == mainPage.currentConversationId)
            var msgId = message.messageId || 0
            // M12.4: 消息写入该会话的视图——当前打开的与后台保留的都要写，
            // 否则切回去只能靠补收页，实时性丢失
            mainPage.appendMessage(convId, message)
            if (active) {
                // M4.5: 会话打开期间收到新消息，发送已读回执
                if (msgId > 0) {
                    networkManager.ackMessage(msgId, "read")
                }
            } else {
                // M4.5: 未打开的会话本地更新预览与未读角标
                mainPage.updateConversationPreview(message)
            }
            // 以服务端为准刷新会话列表（含未读计数）
            networkManager.getConversations()
        }

        // M8.2: 本人发出的文件消息在服务端确认后的本地回显。
        // 两条前提决定了必须由 C++ 回显：服务端的实时 fan-out 明确排除发送者
        //（RequestHandler 里 `if (memberId != operatorId)`），而文件消息又不像
        // 文本消息那样能提前乐观插入——清单要等 hashing 与元数据提取完成才
        // 存在，QML 事先拿不到 sha256/尺寸/缩略图。缺了这条回显，发送方要等
        // 下一次历史同步（手动刷新/重进会话）才看得到自己的文件气泡。
        // 与 onNewMessageReceived 的区别：这是自己发的消息，不回已读回执
        function onFileMessageSent(message) {
            // M12.4: 回显到该会话的视图（发完就切走的会话仍持有其乐观气泡所在的气泡列）
            mainPage.appendMessage(message.conversationId, message)
            // 会话列表不本地拼预览：文件消息的正文已被脱敏出口置空，本地拼出来
            // 是空串；而且自己的消息不该计未读。预览文本与未读数一律以服务端
            // 为准（服务端 lastMessage 经脱敏出口转成 "[File] 文件名"）
            networkManager.getConversations()
        }

        function onMessageSent(messageId, conversationId, clientMessageId) {
            // M12.4: 按幂等键找回发起视图确认乐观气泡（用户可能已切走）；
            // 新会话的首条消息还会把"待绑定"视图迁到服务端会话 ID 上
            mainPage.confirmSentMessage(conversationId, clientMessageId, messageId)
            networkManager.getConversations()
        }

        function onMessageSendFailed(error, clientMessageId) {
            globalToast.error("发送失败：" + error)
            // P2.2: 把对应会话中“发送中”的乐观气泡标记为 failed，供点击重发
            mainPage.handleSendFailed(clientMessageId)
        }

        // 加好友与消息发送失败分开：它不是消息失败，不该碰任何气泡状态
        function onAddContactFailed(error) {
            globalToast.error("添加联系人失败：" + error)
        }

        // M4.5: 消息状态推送（已送达/已读）实时更新气泡状态
        function onMessageStatusChanged(messageId, status) {
            mainPage.updateMessageStatus(messageId, status)
        }

        // P3.2: “正在输入”推送——仅当前打开的会话展示（服务端已排除发起者）
        function onTypingReceived(conversationId, userId, username, typing) {
            if (conversationId == mainPage.currentConversationId) {
                mainPage.showPeerTyping(userId, username, typing)
            }
        }

        function onSearchUsersResult(users) {
            var userList = []
            for (var i = 0; i < users.length; i++) {
                userList.push(users[i])
            }
            mainPage.showSearchResults(userList)
        }

        // M7a: 联系人列表（建群对话框成员选择）
        function onContactsResult(contacts) {
            var contactList = []
            for (var i = 0; i < contacts.length; i++) {
                contactList.push(contacts[i])
            }
            mainPage.showContacts(contactList)
        }

        // M7a: 群组操作结果
        function onGroupCreated(conversationId, name) {
            mainPage.openCreatedGroup(conversationId, name)
            globalToast.success("群组已创建")
        }

        function onGroupLeft(conversationId) {
            mainPage.closeGroupIfCurrent(conversationId)
            globalToast.info("已退出群聊")
        }

        function onGroupInfoResult(info) {
            mainPage.showGroupInfo(info)
        }

        function onGroupRequestFailed(error) {
            globalToast.error("群操作失败：" + error)
        }

        // M7a: 群变更推送：被移除/目标为自己的变更需关闭当前会话
        function onGroupChanged(payload) {
            var changeType = payload.changeType || ""
            var target = payload.targetUserId || 0
            if ((changeType === "member_removed" || changeType === "member_left")
                && target === mainWindow.myUserId) {
                mainPage.closeGroupIfCurrent(payload.conversationId || 0)
            }
        }

        // M9 特性栈：会话偏好推送（本人其他设备设置后同步）
        function onConversationPrefsChanged(conversationId, pinned, muted) {
            mainPage.applyConversationPrefs(conversationId, pinned, muted)
        }

        // M9 特性栈：消息编辑结果（本端响应或其他成员推送）
        function onMessageEdited(conversationId, messageId, content, editedAt) {
            mainPage.applyMessageEdited(conversationId, messageId, content, editedAt)
        }

        // M9 特性栈：消息删除结果（本端响应或其他成员推送）
        function onMessageDeleted(conversationId, messageId) {
            mainPage.applyMessageDeleted(conversationId, messageId)
        }

        function onMessageEditFailed(error) {
            globalToast.error("编辑失败：" + error)
        }

        function onMessageDeleteFailed(error) {
            globalToast.error("删除失败：" + error)
        }

        // P3.1: 会话删除结果（本端响应或其他成员/设备推送）
        function onConversationDeleted(conversationId) {
            mainPage.handleConversationDeleted(conversationId)
            globalToast.info("会话已删除")
        }

        function onConversationDeleteFailed(error) {
            globalToast.error("删除会话失败：" + error)
        }
    }

    // M10: 全局 Toast 反馈层（覆盖在所有内容之上）
    Toast {
        id: globalToast
        anchors.fill: parent
    }

    // M4.5: 对当前会话中最后一条对方消息发送已读回执
    function sendReadAck() {
        var lastIncomingId = mainPage.lastIncomingMessageId()
        if (lastIncomingId > 0) {
            networkManager.ackMessage(lastIncomingId, "read")
        }
    }

    // 公共方法
    function loadConversations() {
        networkManager.getConversations()
    }

    // P2: 会话失效回登录页时重置主页面 UI（会话列表/聊天区/当前会话选择）
    function resetUi() {
        mainPage.resetUi()
    }
}
