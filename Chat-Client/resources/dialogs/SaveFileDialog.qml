import QtQuick.Dialogs

FileDialog {
    id: root

    property int messageId: 0
    signal saveAccepted(int messageId, var fileUrl)

    title: "保存文件到"
    fileMode: FileDialog.SaveFile

    onAccepted: {
        if (root.messageId > 0) {
            root.saveAccepted(root.messageId, selectedFile)
        }
        root.messageId = 0
    }

    onRejected: root.messageId = 0
}
