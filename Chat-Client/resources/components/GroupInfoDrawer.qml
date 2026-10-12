import QtQuick
import QtQuick.Controls
import "../theme"

// M11C C1: 群信息抽屉——替代原弹出式 GroupInfoDialog，从右缘滑出。
// 非模态、无遮罩：浏览成员时不阻断聊天区操作，与 C3 常驻右栏同一交互口径。
// closePolicy 只留 Escape：默认的 CloseOnPressOutside 会让"边聊边看"在第一次
// 点击聊天区时就收起，切换会话跟随换载也随之失效
Drawer {
    id: root

    edge: Qt.RightEdge
    modal: false
    dim: false
    closePolicy: Popup.CloseOnEscape
    // 未填充（convId=0）时禁止从右缘拖出空抽屉——此时邀请/退出都作用于 0 号会话
    interactive: panel.convId > 0
    width: panel.implicitWidth

    property int myUserId: 0
    readonly property int convId: panel.convId

    signal inviteClicked()
    signal leaveRequested(int conversationId)
    signal kickRequested(int conversationId, int userId)
    signal refreshRequested(int conversationId)

    background: Rectangle {
        color: Theme.windowBackground

        Rectangle {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: 1
            color: Theme.separatorColor
        }
    }

    contentItem: GroupInfoPanel {
        id: panel
        myUserId: root.myUserId
        onCloseRequested: root.close()
        onInviteClicked: root.inviteClicked()
        onLeaveRequested: function(conversationId) { root.leaveRequested(conversationId) }
        onKickRequested: function(conversationId, userId) { root.kickRequested(conversationId, userId) }
        onRefreshRequested: function(conversationId) { root.refreshRequested(conversationId) }
    }

    function showInfo(info) {
        panel.showInfo(info)
        if (!opened) open()
    }
}
