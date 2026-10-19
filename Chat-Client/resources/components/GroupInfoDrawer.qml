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
    // 顶边让开自定义标题栏：无边框窗口的标题栏承载拖拽与窗口控制按钮（最小化/
    // 最大化/关闭），抽屉若从 y=0 铺满整个窗口高度会连带把它们盖住。Popup 默认
    // 以窗口 overlay 为父（坐标系即整个窗口），故显式给出纵向范围
    y: Theme.titleBarHeight
    height: parent ? parent.height - Theme.titleBarHeight : 0

    property int myUserId: 0
    readonly property int convId: panel.convId

    signal inviteClicked()
    signal leaveRequested(int conversationId)
    signal kickRequested(int conversationId, int userId)

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
    }

    // M11C C3: 只负责填充内容，开场由调用方决定——宽屏走常驻右栏（不使用本抽屉），
    // 窄屏才 open()。抽屉若自行 open()，宽屏下会被"填充"意外拉出来
    function showInfo(info) {
        panel.showInfo(info)
    }
}
