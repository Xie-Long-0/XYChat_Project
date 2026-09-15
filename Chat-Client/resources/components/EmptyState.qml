import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import "../theme"

ColumnLayout {
    id: root

    property string iconName: ""
    property string title: ""
    property string subtitle: ""
    property string actionText: ""
    signal actionClicked()

    spacing: Theme.spacingMedium

    Rectangle {
        Layout.alignment: Qt.AlignHCenter
        width: 80; height: 80
        radius: 40
        color: Theme.primaryLightColor
        visible: root.iconName.length > 0

        Icon {
            anchors.centerIn: parent
            name: root.iconName
            size: 32
            iconColor: Theme.primaryColor
        }
    }

    Label {
        Layout.alignment: Qt.AlignHCenter
        text: root.title
        font.pixelSize: Theme.fontSizeSubtitle
        color: Theme.textSecondary
        visible: text.length > 0
        horizontalAlignment: Text.AlignHCenter
    }

    Label {
        Layout.alignment: Qt.AlignHCenter
        text: root.subtitle
        font.pixelSize: Theme.fontSizeCaption
        color: Theme.textTertiary
        visible: text.length > 0
        horizontalAlignment: Text.AlignHCenter
    }

    AppButton {
        Layout.alignment: Qt.AlignHCenter
        text: root.actionText
        variant: "flat"
        visible: root.actionText.length > 0
        onClicked: root.actionClicked()
    }
}
