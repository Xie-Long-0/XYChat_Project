import QtQuick
import QtQuick.Controls
import "../theme"

Button {
    id: root

    property string variant: "primary"

    font.pixelSize: Theme.fontSizeBody
    font.weight: Font.DemiBold

    background: Rectangle {
        radius: Theme.radiusSmall
        color: {
            if (!root.enabled) return Theme.textTertiary
            if (root.variant === "primary")
                return root.pressed ? Theme.loginButtonPressed
                     : (root.hovered ? Theme.loginButtonHover : Theme.primaryColor)
            if (root.variant === "danger")
                return root.pressed ? Theme.loginErrorColor
                     : (root.hovered ? Theme.errorColor : Theme.unreadBadgeColor)
            if (root.variant === "secondary")
                return root.pressed ? Theme.pressedColor
                     : (root.hovered ? Theme.hoverColor : "transparent")
            return "transparent"
        }
        Behavior on color { ColorAnimation { duration: Theme.animationFast; easing.type: Theme.easingStandard } }
    }

    contentItem: Label {
        text: root.text
        font: root.font
        color: {
            if (!root.enabled) return Theme.textTertiary
            if (root.variant === "primary" || root.variant === "danger")
                return Theme.textOnPrimary
            if (root.variant === "secondary")
                return Theme.textPrimary
            return Theme.primaryColor
        }
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
}
