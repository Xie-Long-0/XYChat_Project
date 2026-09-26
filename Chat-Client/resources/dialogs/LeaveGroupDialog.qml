import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../theme"
import "../components"

// M13: 退群前的本机历史去向选择（对齐主流 IM 的“退出 / 退出并删除聊天记录”）。
// 两个去向只影响本机：服务端只做成员移除，群内历史对其他成员不受影响
AppDialog {
    id: root

    property int conversationId: 0
    property string groupName: ""

    signal leaveOnlyConfirmed(int conversationId)
    signal leaveAndDeleteConfirmed(int conversationId)

    title: "退出群聊"
    width: 380

    contentItem: ColumnLayout {
        spacing: Theme.spacingLarge

        Label {
            Layout.fillWidth: true
            text: "退出「" + (root.groupName || "该群聊")
                  + "」后你将不再接收该群消息。请选择本机聊天记录的处理方式："
            font.pixelSize: Theme.fontSizeBody
            color: Theme.textPrimary
            wrapMode: Text.Wrap
        }

        Label {
            Layout.fillWidth: true
            text: "仅退出：会话与历史保留在列表中，可继续查看（只读）。\n"
                  + "退出并删除记录：清空本机聊天记录并从列表移除，其他成员不受影响。"
            font.pixelSize: Theme.fontSizeSmall
            color: Theme.textSecondary
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
                text: "仅退出"
                variant: "secondary"
                onClicked: {
                    root.leaveOnlyConfirmed(root.conversationId)
                    root.close()
                }
            }

            AppButton {
                text: "退出并删除记录"
                variant: "danger"
                onClicked: {
                    root.leaveAndDeleteConfirmed(root.conversationId)
                    root.close()
                }
            }
        }
    }

    // 关闭后复位，避免下次打开残留上一次的会话与群名
    onClosed: {
        root.conversationId = 0
        root.groupName = ""
    }
}
