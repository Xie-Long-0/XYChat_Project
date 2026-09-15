import QtQuick
import QtQuick.Controls
import "../theme"

TextField {
    id: root

    property bool hasError: false

    font.pixelSize: Theme.fontSizeBody
    color: Theme.textPrimary
    placeholderTextColor: Theme.inputPlaceholderColor
    selectByMouse: true

    background: Rectangle {
        radius: Theme.radiusSmall
        color: Theme.inputBackground
        border.width: root.activeFocus ? 2 : 1
        border.color: {
            if (root.hasError) return Theme.errorColor
            return root.activeFocus ? Theme.inputFocusBorderColor : Theme.inputBorderColor
        }
        Behavior on border.color { ColorAnimation { duration: Theme.animationFast; easing.type: Theme.easingStandard } }
    }
}
