import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../theme"
import "../components"

AppDialog {
    id: root

    property int messageId: 0
    signal saveRequested(int messageId, string newContent)

    title: "编辑消息"
    width: 400

    contentItem: ColumnLayout {
        spacing: Theme.spacingMedium

        AppTextField {
            id: editField
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.inputHeight
            placeholderText: "输入新内容..."
        }

        Label {
            id: editStatus
            text: ""
            font.pixelSize: Theme.fontSizeCaption
            color: Theme.textSecondary
            visible: text !== ""
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingSmall

            Item { Layout.fillWidth: true }

            AppButton {
                text: "取消"
                variant: "flat"
                onClicked: root.close()
            }

            AppButton {
                text: "保存"
                onClicked: root.doEdit()
            }
        }
    }

    function openFor(msgId, content) {
        root.messageId = msgId
        editField.text = content
        editStatus.text = ""
        open()
    }

    function doEdit() {
        var text = editField.text.trim()
        if (text.length === 0) {
            editStatus.text = "内容不能为空"
            return
        }
        if (root.messageId <= 0) {
            editStatus.text = "消息 ID 无效"
            return
        }
        root.saveRequested(root.messageId, text)
        close()
    }

    onClosed: {
        editField.text = ""
        root.messageId = 0
        editStatus.text = ""
    }
}
