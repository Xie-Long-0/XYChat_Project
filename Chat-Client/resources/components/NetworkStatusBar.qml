import QtQuick
import QtQuick.Controls
import "../theme"

Rectangle {
    id: root

    property int networkState: 4

    height: visible ? 24 : 0
    visible: networkState !== 4
    clip: true

    color: {
        if (networkState === 0) return Theme.errorColor
        if (networkState === 1 || networkState === 3) return Theme.warningColor
        return Theme.infoColor
    }

    Behavior on height {
        NumberAnimation { duration: Theme.animationNormal; easing.type: Theme.easingStandard }
    }

    Label {
        anchors.centerIn: parent
        text: {
            if (root.networkState === 0) return "未连接"
            if (root.networkState === 1) return "连接中..."
            if (root.networkState === 2) return "已连接"
            if (root.networkState === 3) return "登录中..."
            return ""
        }
        font.pixelSize: Theme.fontSizeCaption
        color: Theme.textOnPrimary
    }
}
