import QtQuick
import QtQuick.Controls
import "../theme"

Rectangle {
    id: root

    property int userId: 0
    property string name: ""
    property real size: Theme.avatarSize
    property bool isGroup: false
    property bool online: false

    width: size
    height: size
    radius: isGroup ? Theme.radiusMedium : size / 2
    color: Theme.avatarColor(userId)

    Label {
        anchors.centerIn: parent
        text: root.name.length > 0 ? root.name[0].toUpperCase() : "?"
        font.pixelSize: root.size * 0.4
        font.weight: Font.Bold
        color: Theme.textOnPrimary
    }

    Rectangle {
        visible: root.online
        width: root.size * 0.25
        height: root.size * 0.25
        radius: width / 2
        color: Theme.successColor
        border.width: 2
        border.color: Theme.windowBackground
        anchors.right: parent.right
        anchors.bottom: parent.bottom
    }
}
