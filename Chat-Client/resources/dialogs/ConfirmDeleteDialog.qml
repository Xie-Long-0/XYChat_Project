import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../theme"
import "../components"

AppDialog {
    id: root

    // 三选一：messageIds 非空为批量消息删除（M11B B1 多选）；messageId>0 为
    // 单条消息删除；conversationId>0 为会话整表删除（localOnly 时改为
    // 仅删本机历史——已退出的群会话服务端会拒绝整表删除，见 M13）
    property int messageId: 0
    property int conversationId: 0
    property var messageIds: []
    property bool localOnly: false
    // 文案由调用方在 open 前设置（默认为消息删除文案）
    readonly property string messageDeleteBody: "删除后所有会话成员都将看到“消息已删除”，此操作不可撤销。是否继续？"
    property string titleText: "删除消息"
    property string bodyText: messageDeleteBody

    signal confirmed(int messageId)
    signal conversationConfirmed(int conversationId)
    signal localConversationConfirmed(int conversationId)
    signal batchConfirmed(var messageIds)

    title: root.titleText
    width: 320

    contentItem: ColumnLayout {
        spacing: Theme.spacingLarge

        Label {
            Layout.fillWidth: true
            text: root.bodyText
            font.pixelSize: Theme.fontSizeBody
            color: Theme.textPrimary
            wrapMode: Text.Wrap
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
                text: "删除"
                variant: "danger"
                onClicked: {
                    if (root.messageIds.length > 0) {
                        root.batchConfirmed(root.messageIds)
                    } else if (root.localOnly && root.conversationId > 0) {
                        root.localConversationConfirmed(root.conversationId)
                    } else if (root.conversationId > 0) {
                        root.conversationConfirmed(root.conversationId)
                    } else {
                        root.confirmed(root.messageId)
                    }
                    root.close()
                }
            }
        }
    }

    // 关闭后复位，避免下次打开残留上一次的 id/批次与文案
    onClosed: {
        root.messageId = 0
        root.conversationId = 0
        root.messageIds = []
        root.localOnly = false
        root.titleText = "删除消息"
        root.bodyText = root.messageDeleteBody
    }
}
