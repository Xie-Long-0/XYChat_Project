import QtQuick
import QtQuick.Controls
import "../theme"

Item {
    id: root

    anchors.fill: parent
    z: 1000

    function show(msg, variant, dur) {
        toastLabel.text = msg
        var v = variant || "info"
        if (v === "success") toastRect.color = Theme.successColor
        else if (v === "error") toastRect.color = Theme.errorColor
        else if (v === "warning") toastRect.color = Theme.warningColor
        else toastRect.color = Theme.infoColor
        toastRect.opacity = 1.0
        hideTimer.interval = dur || 3000
        hideTimer.restart()
    }

    function success(msg) { show(msg, "success") }
    function error(msg) { show(msg, "error") }
    function info(msg) { show(msg, "info") }
    function warning(msg) { show(msg, "warning") }

    Timer {
        id: hideTimer
        interval: 3000
        onTriggered: toastRect.opacity = 0
    }

    Rectangle {
        id: toastRect
        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottomMargin: Theme.spacingXLarge
        width: Math.min(toastLabel.implicitWidth + Theme.spacingLarge * 2,
                        parent.width - Theme.spacingXLarge * 2)
        height: toastLabel.implicitHeight + Theme.spacingMedium * 2
        radius: Theme.radiusMedium
        opacity: 0

        Behavior on opacity {
            NumberAnimation { duration: Theme.animationNormal; easing.type: Theme.easingStandard }
        }

        Label {
            id: toastLabel
            anchors.centerIn: parent
            width: parent.width - Theme.spacingLarge * 2
            font.pixelSize: Theme.fontSizeBody
            color: Theme.textOnPrimary
            wrapMode: Text.Wrap
            horizontalAlignment: Text.AlignHCenter
        }
    }
}
