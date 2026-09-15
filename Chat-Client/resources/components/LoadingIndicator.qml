import QtQuick
import "../theme"

Icon {
    id: root

    property bool running: true

    name: "refresh"
    size: 24
    iconColor: Theme.primaryColor

    RotationAnimator {
        target: root
        from: 0
        to: 360
        duration: 1000
        loops: Animation.Infinite
        running: root.running && root.visible
    }
}
