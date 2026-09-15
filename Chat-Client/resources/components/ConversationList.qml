import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "../theme"

Rectangle {
    id: conversationList
    color: Theme.sidebarBackground

    signal conversationClicked(int index)
    signal searchClicked()
    signal refreshClicked()
    signal createGroupClicked()
    signal conversationPrefsRequested(int conversationId, bool pinned, bool muted)
    signal markReadRequested(int conversationId)
    signal deleteConversationRequested(int conversationId)

    // M4.5: 当前选中会话索引（修复原先错误的判断条件）
    property int selectedIndex: -1

    // 会话列表是否处于「已发起拉取、尚未收到结果」状态。
    // 初值为 true：登录后 MainPage/MainWindow 会立刻拉取会话，收到结果时由
    // updateConversations() 置为 false。
    // 借此把「加载中」与「加载完成但确实没有任何会话」区分开——此前空列表
    // 只按 convModel.count === 0 判断，导致没有会话时永远显示"加载中..."。
    property bool loading: true

    // 顶部工具栏
    Rectangle {
        id: toolbar
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 52
        color: Theme.sidebarBackground

        // 底部分隔线
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

            // 搜索按钮
            Rectangle {
                id: searchBtn
                width: 36; height: 36
                radius: 18
                color: searchMouse.containsMouse ? Theme.hoverColor : "transparent"

                MouseArea {
                    id: searchMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: conversationList.searchClicked()
                }

                Icon {
                    anchors.centerIn: parent
                    name: "search"
                    size: 16
                    iconColor: Theme.textSecondary
                }
            }

            // 标题
            Label {
                Layout.fillWidth: true
                text: "会话"
                font.pixelSize: Theme.fontSizeXLarge
                font.weight: Font.Bold
                color: Theme.textPrimary
            }

            // M7a: 建群按钮
            Rectangle {
                id: createGroupBtn
                width: 36; height: 36
                radius: 18
                color: createGroupMouse.containsMouse ? Theme.hoverColor : "transparent"

                MouseArea {
                    id: createGroupMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: conversationList.createGroupClicked()
                }

                Icon {
                    anchors.centerIn: parent
                    name: "plus"
                    size: 14
                    iconColor: Theme.textSecondary
                }
            }

            // 刷新按钮
            Rectangle {
                id: refreshBtn
                width: 36; height: 36
                radius: 18
                color: refreshMouse.containsMouse ? Theme.hoverColor : "transparent"

                MouseArea {
                    id: refreshMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        // 列表为空时点刷新，先回到加载态显示 spinner
                        conversationList.beginLoading()
                        conversationList.refreshClicked()
                    }
                }

                Icon {
                    anchors.centerIn: parent
                    name: "refresh"
                    size: 16
                    iconColor: Theme.textSecondary
                }
            }
        }
    }

    // 会话列表
    ListView {
        id: listView
        anchors.top: toolbar.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        clip: true

        model: ListModel { id: convModel }

        delegate: Rectangle {
            id: delegateItem
            width: listView.width
            height: Theme.conversationItemHeight
            property bool isSelected: index === conversationList.selectedIndex
            color: isSelected ? Theme.selectedConversationColor
                 : (delegateMouse.pressed ? Theme.pressedColor
                    : (delegateMouse.containsMouse ? Theme.hoverColor : "transparent"))

            Behavior on color { ColorAnimation { duration: Theme.animationFast } }

            MouseArea {
                id: delegateMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                onClicked: function(mouse) {
                    if (mouse.button === Qt.RightButton) {
                        convContextMenu.popup()
                    } else {
                        conversationList.conversationClicked(index)
                    }
                }
            }

            // 会话右键菜单。用 AppMenu/AppMenuItem 而非裸 Menu/MenuItem
            //（Basic 样式在暗色主题下是浅色面板 + 不可见的白图标，见两个组件的说明）
            AppMenu {
                id: convContextMenu
                AppMenuItem {
                    text: model.pinned === true ? "取消置顶" : "置顶会话"
                    iconName: "pin"
                    onTriggered: conversationList.conversationPrefsRequested(
                        model.conversationId, model.pinned !== true, model.muted === true)
                }
                AppMenuItem {
                    text: model.muted === true ? "取消免打扰" : "开启免打扰"
                    iconName: model.muted === true ? "bell" : "mute"
                    onTriggered: conversationList.conversationPrefsRequested(
                        model.conversationId, model.pinned === true, model.muted !== true)
                }
                AppMenuItem {
                    text: "标记为已读"
                    iconName: "check"
                    onTriggered: conversationList.markReadRequested(model.conversationId)
                }
                // 分隔线同样要主题化：Basic 的 MenuSeparator 取 palette.mid（系统浅色），
                // 暗色主题下是一条亮线
                MenuSeparator {
                    contentItem: Rectangle {
                        implicitWidth: 160
                        implicitHeight: 1
                        color: Theme.separatorColor
                    }
                }
                AppMenuItem {
                    text: "删除会话"
                    iconName: "delete"
                    danger: true
                    onTriggered: conversationList.deleteConversationRequested(model.conversationId)
                }
            }

            // 底部分隔线
            Rectangle {
                anchors.bottom: parent.bottom
                anchors.left: parent.left
                anchors.leftMargin: Theme.avatarSize + Theme.spacingMedium * 2
                anchors.right: parent.right
                height: 1
                color: Theme.dividerColor
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.spacingMedium
                anchors.rightMargin: Theme.spacingMedium
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.spacingSmall

                Avatar {
                    userId: model.type === "group" ? (model.conversationId || 0)
                                                  : (model.peerUserId || 0)
                    name: model.displayName
                    size: Theme.avatarSize
                    isGroup: model.type === "group"
                }

                // 信息区域
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    spacing: Theme.spacingXSmall

                    // 名称行
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 0

                        Label {
                            Layout.fillWidth: true
                            text: model.displayName
                            font.pixelSize: Theme.fontSizeMedium
                            font.weight: Font.DemiBold
                            color: delegateItem.isSelected ? Theme.selectedConversationTextColor : Theme.textPrimary
                            elide: Text.ElideRight
                        }

                        // M7a: 群成员数标识
                        Label {
                            visible: model.type === "group"
                            text: model.memberCount + "人"
                            font.pixelSize: Theme.fontSizeSmall - 1
                            color: delegateItem.isSelected
                                 ? Theme.selectedConversationSecondaryColor
                                 : Theme.textTertiary
                        }

                        // M9 特性栈：免打扰标识
                        Icon {
                            visible: model.muted === true
                            name: "mute"
                            size: 12
                            iconColor: delegateItem.isSelected
                                 ? Theme.selectedConversationSecondaryColor
                                 : Theme.textTertiary
                        }

                        Label {
                            text: model.lastMessageTime || ""
                            font.pixelSize: Theme.fontSizeSmall - 1
                            color: delegateItem.isSelected
                                 ? Theme.selectedConversationSecondaryColor
                                 : (model.unreadCount > 0 ? Theme.primaryColor : Theme.textTertiary)
                        }

                        // M9 特性栈：置顶标识
                        Icon {
                            visible: model.pinned === true
                            name: "pin"
                            size: 12
                            iconColor: delegateItem.isSelected
                                 ? Theme.selectedConversationSecondaryColor
                                 : Theme.primaryColor
                        }
                    }

                    // 消息预览行
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 0

                        Label {
                            Layout.fillWidth: true
                            text: model.lastMessage || ""
                            font.pixelSize: Theme.fontSizeSmall
                            color: delegateItem.isSelected ? Theme.selectedConversationSecondaryColor : Theme.textSecondary
                            elide: Text.ElideRight
                            maximumLineCount: 1
                        }

                        // 未读角标
                        Rectangle {
                            visible: model.unreadCount > 0
                            width: unreadLabel.implicitWidth + 10
                            height: 20
                            radius: 10
                            color: Theme.unreadBadgeColor

                            Label {
                                id: unreadLabel
                                anchors.centerIn: parent
                                text: model.unreadCount > 99 ? "99+" : model.unreadCount
                                font.pixelSize: Theme.fontSizeSmall - 1
                                font.weight: Font.DemiBold
                                color: Theme.unreadBadgeTextColor
                            }
                        }
                    }
                }
            }
        }

        // 空状态提示：loading 为 true 时显示 spinner（"加载中..."），
        // 拉取完成但一条会话都没有时显示空态图标 +"暂无会话"
        Column {
            anchors.centerIn: parent
            spacing: Theme.spacingMedium
            visible: convModel.count === 0

            LoadingIndicator {
                anchors.horizontalCenter: parent.horizontalCenter
                size: 24
                visible: conversationList.loading
                running: visible
            }

            // 空态图标。Column 会跳过不可见子项，故两个图标不会同时占位
            Icon {
                anchors.horizontalCenter: parent.horizontalCenter
                visible: !conversationList.loading
                name: "chat-bubble"
                size: 32
                iconColor: Theme.textTertiary
            }

            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                text: conversationList.loading ? "加载中..." : "暂无会话"
                horizontalAlignment: Text.AlignHCenter
                font.pixelSize: Theme.fontSizeMedium
                color: Theme.textTertiary
            }
        }
    }

    // 公共方法
    // M4.5: 会话时间格式化（服务端字段为 ISO 时间 lastMessageAt）
    function formatConvTime(isoStr) {
        if (!isoStr || isoStr.length === 0) return ""
        var d = new Date(isoStr)
        if (isNaN(d.getTime())) return ""
        var now = new Date()
        if (d.toDateString() === now.toDateString()) {
            var hh = d.getHours()
            var mm = d.getMinutes()
            return (hh < 10 ? "0" : "") + hh + ":" + (mm < 10 ? "0" : "") + mm
        }
        return d.getFullYear() + "/" + (d.getMonth() + 1) + "/" + d.getDate()
    }

    // 将 conversationId 统一为 Number，避免 QJsonArray/QML 模型中 number/string 混用导致 === 匹配失败
    function normalizeConversationId(id) {
        var n = Number(id)
        return isNaN(n) ? 0 : n
    }

    function findIndexByConversationId(conversationId) {
        var target = normalizeConversationId(conversationId)
        for (var i = 0; i < convModel.count; i++) {
            if (normalizeConversationId(convModel.get(i).conversationId) === target) {
                return i
            }
        }
        return -1
    }

    function updateConversations(conversations) {
        if (!conversations || conversations.length === undefined) {
            return
        }
        // 收到任一有效会话结果即视为本次拉取结束（空数组同样算结束，
        // 此时由下方空状态显示"暂无会话"而不是一直转圈）
        loading = false
        var seen = {}
        for (var i = 0; i < conversations.length; i++) {
            var conv = conversations[i]
            if (!conv || conv.conversationId === undefined) {
                continue
            }
            var convId = normalizeConversationId(conv.conversationId)
            seen[convId] = true
            var type = conv.type || "private"
            var entry = {
                conversationId: convId,
                type: type,
                peerUserId: normalizeConversationId(conv.peerUserId),
                peerUsername: conv.peerUsername || "",
                // M7a: 群会话显示群名与成员数
                name: conv.name || "",
                memberCount: conv.memberCount || 0,
                displayName: type === "group" ? (conv.name || "未命名群组")
                                              : (conv.peerUsername || ""),
                lastMessage: conv.lastMessage || "",
                lastMessageTime: formatConvTime(conv.lastMessageAt || ""),
                unreadCount: conv.unreadCount || 0,
                // M9 特性栈：会话偏好（置顶/免打扰）
                pinned: conv.pinned === true,
                muted: conv.muted === true
            }
            var pos = findIndexByConversationId(convId)
            if (pos >= 0) {
                convModel.set(pos, entry)
            } else {
                convModel.append(entry)
            }
        }
        // 移除服务端已不存在的会话（退群/被移出后列表同步消失）
        for (var j = convModel.count - 1; j >= 0; j--) {
            if (!(normalizeConversationId(convModel.get(j).conversationId) in seen)) {
                // Qt 部分版本 ListModel.remove 要求显式 count
                convModel.remove(j, 1)
            }
        }
        // 按服务端顺序重排当前列表（插入排序式单步移动，k 递增且 move 目标 <= k 可保证正确性）
        for (var k = 0; k < conversations.length; k++) {
            var convK = conversations[k]
            if (!convK || convK.conversationId === undefined) {
                continue
            }
            var idx = findIndexByConversationId(normalizeConversationId(convK.conversationId))
            if (idx >= 0 && idx !== k) {
                // Qt 部分版本 ListModel.move 要求三个参数，显式传入 count=1
                convModel.move(idx, k, 1)
            }
        }
    }

    function getConversation(index) {
        if (index >= 0 && index < convModel.count) {
            return convModel.get(index)
        }
        return null
    }

    // M4.5: 按会话 ID 选中（会话刷新后恢复高亮）
    function setSelectedByConversationId(conversationId) {
        var target = normalizeConversationId(conversationId)
        selectedIndex = -1
        for (var i = 0; i < convModel.count; i++) {
            if (normalizeConversationId(convModel.get(i).conversationId) === target) {
                selectedIndex = i
                return
            }
        }
    }

    // M4.5: 按对方用户 ID 查找既有会话（搜索发起对话时复用）
    function findConversationByPeerId(peerUserId) {
        var target = normalizeConversationId(peerUserId)
        for (var i = 0; i < convModel.count; i++) {
            if (normalizeConversationId(convModel.get(i).peerUserId) === target) {
                return convModel.get(i)
            }
        }
        return null
    }

    // M4.5: 新消息到达时本地更新预览与未读角标（当前打开的会话不计未读）
    function updateForNewMessage(conversationId, preview, timeStr, incrementUnread) {
        var target = normalizeConversationId(conversationId)
        for (var i = 0; i < convModel.count; i++) {
            if (normalizeConversationId(convModel.get(i).conversationId) === target) {
                convModel.setProperty(i, "lastMessage", preview)
                convModel.setProperty(i, "lastMessageTime", timeStr)
                if (incrementUnread) {
                    convModel.setProperty(i, "unreadCount", convModel.get(i).unreadCount + 1)
                }
                return
            }
        }
    }

    // 清空选中与列表（登出时）。loading 复位为 true：下一个会话登录后会重新拉取
    function reset() {
        convModel.clear()
        selectedIndex = -1
        loading = true
    }

    // 重新发起拉取时调用，回到加载态（刷新按钮、建群/退群后重载等）。
    // 列表非空时不会看到 spinner——空状态整块仅在 convModel.count === 0 时可见
    function beginLoading() {
        loading = true
    }

    // P3.1: 会话整表删除后从列表移除该项（并修正选中索引）
    function removeConversation(conversationId) {
        var idx = findIndexByConversationId(conversationId)
        if (idx < 0) {
            return
        }
        convModel.remove(idx, 1)
        if (selectedIndex === idx) {
            selectedIndex = -1
        } else if (selectedIndex > idx) {
            selectedIndex = selectedIndex - 1
        }
    }

    // M9 特性栈：本地应用会话偏好（服务端推送 conversation_prefs 后回填），
    // 置顶变更时按 pinned DESC 稳定重排（置顶在前，保持各自相对顺序）
    function applyPrefs(conversationId, pinned, muted) {
        var idx = findIndexByConversationId(conversationId)
        if (idx < 0) {
            return
        }
        convModel.setProperty(idx, "pinned", pinned === true)
        convModel.setProperty(idx, "muted", muted === true)
        reorderByPinned()
    }

    // 稳定分区：置顶会话在前、未置顶在后，各自保持原有相对顺序（单步 move 升序移动）
    function reorderByPinned() {
        var order = []
        // 先收集置顶
        for (var i = 0; i < convModel.count; i++) {
            if (convModel.get(i).pinned === true) {
                order.push(normalizeConversationId(convModel.get(i).conversationId))
            }
        }
        // 再收集未置顶
        for (var j = 0; j < convModel.count; j++) {
            if (convModel.get(j).pinned !== true) {
                order.push(normalizeConversationId(convModel.get(j).conversationId))
            }
        }
        for (var k = 0; k < order.length; k++) {
            var cur = normalizeConversationId(convModel.get(k).conversationId)
            if (cur !== order[k]) {
                var target = findIndexByConversationId(order[k])
                if (target > k) {
                    convModel.move(target, k, 1)
                }
            }
        }
    }
}
