import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../theme"
import "../components"

AppDialog {
    id: root

    property int messageId: 0
    property var messageInfo: null
    signal saveRequested(int messageId)

    title: {
        if (!root.messageInfo || !root.messageInfo.fileName
            || root.messageInfo.fileName.length === 0) {
            return "图片预览"
        }
        var suffix = (root.messageInfo.fileWidth > 0 && root.messageInfo.fileHeight > 0)
                     ? "  ·  " + root.messageInfo.fileWidth + "×" + root.messageInfo.fileHeight
                     : ""
        return root.messageInfo.fileName + suffix
    }

    width: Math.min(parent.width * 0.85, 900)
    height: parent.height * 0.85
    standardButtons: Dialog.NoButton

    onRejected: root.messageId = 0

    contentItem: Item {
        Image {
            id: previewImage
            anchors.fill: parent
            source: root.messageId > 0
                    ? "image://xyfile/" + root.messageId : ""
            fillMode: Image.PreserveAspectFit
            asynchronous: true
            smooth: true
            cache: false
        }

        LoadingIndicator {
            anchors.centerIn: parent
            size: 32
            running: previewImage.status === Image.Loading
            visible: running
        }

        Label {
            anchors.centerIn: parent
            width: parent.width * 0.8
            visible: previewImage.status === Image.Error
                     || previewImage.status === Image.Null
            text: "无法预览：文件未就绪、不是图片，或已超过内存解码上限（64 MB）。可改用“另存为”。"
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSizeCaption
            wrapMode: Text.Wrap
            horizontalAlignment: Text.AlignHCenter
        }
    }

    footer: DialogButtonBox {
        AppButton {
            text: "另存为"
            DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
            onClicked: {
                if (root.messageId > 0) {
                    root.saveRequested(root.messageId)
                }
            }
        }
        AppButton {
            text: "关闭"
            variant: "flat"
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
        }
    }
}
