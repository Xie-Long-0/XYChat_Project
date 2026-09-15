import QtQuick
import QtQuick.Controls
import "../theme"

Dialog {
    id: root

    modal: true
    anchors.centerIn: parent
    padding: Theme.spacingLarge

    background: Rectangle {
        radius: Theme.radiusLarge
        color: Theme.windowBackground
        border.width: 1
        border.color: Theme.borderColor
    }

    header: Label {
        text: root.title
        font.pixelSize: Theme.fontSizeHeading
        font.weight: Font.DemiBold
        color: Theme.textPrimary
        padding: Theme.spacingLarge
        bottomPadding: 0
        visible: root.title.length > 0
    }
}
