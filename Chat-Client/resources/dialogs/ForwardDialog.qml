import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "../theme"
import "../components"

AppDialog {
    id: root

    title: "转发到"
    width: 380
    height: 440

    // 待转发的消息 ID（原样回传，供发起方核对与提示）与会话快照（由 MainPage
    // 注入 convList.conversationSummaries()：对话框是模态的，快照即最终候选集）
    property var messageIds: []
    property var conversations: []
    // 目标选中态（-1 表示未选；只在本次打开内有效，onOpened 复位）
    property int selectedConversationId: -1
    readonly property string filterText: searchField.text.toLowerCase()
    // 当前选中项（ListModel 行副本），确认时用来回传会话类型与对方用户 ID
    readonly property var selectedTarget: {
        for (var i = 0; i < targetModel.count; i++) {
            var c = targetModel.get(i)
            if (c.conversationId === selectedConversationId) return c
        }
        return null
    }

    signal forwardConfirmed(var messageIds, int conversationId, bool isGroup, int peerUserId)

    onOpened: {
        selectedConversationId = -1
        targetModel.clear()
        for (var i = 0; i < conversations.length; i++) {
            var c = conversations[i]
            targetModel.append({
                conversationId: c.conversationId,
                type: c.type || "private",
                peerUserId: c.peerUserId || 0,
                displayName: c.displayName || "",
                memberCount: c.memberCount || 0
            })
        }
        searchField.text = ""
        searchField.forceActiveFocus()
    }

    contentItem: ColumnLayout {
        spacing: Theme.spacingMedium

        AppTextField {
            id: searchField
            Layout.fillWidth: true
            placeholderText: "搜索会话..."
        }

        ListView {
            id: targetList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: ListModel { id: targetModel }

            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            delegate: Rectangle {
                id: targetItem
                width: targetList.width
                // 不匹配搜索词的行折叠为 0 高（比另建过滤模型简单，且行数上限
                // 就是会话数，量级很小）
                readonly property bool matches: root.filterText.length === 0
                    || model.displayName.toLowerCase().indexOf(root.filterText) >= 0
                height: matches ? 60 : 0
                visible: matches
                color: model.conversationId === root.selectedConversationId
                       ? Theme.primaryLightColor
                       : (targetMouse.containsMouse ? Theme.hoverColor : "transparent")
                radius: Theme.radiusSmall

                Behavior on color { ColorAnimation { duration: Theme.animationFast } }

                MouseArea {
                    id: targetMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.selectedConversationId = model.conversationId
                    onDoubleClicked: {
                        root.selectedConversationId = model.conversationId
                        root.confirmSelection()
                    }
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spacingSmall
                    anchors.rightMargin: Theme.spacingSmall
                    spacing: Theme.spacingSmall

                    Avatar {
                        userId: model.type === "group" ? model.conversationId : model.peerUserId
                        name: model.displayName
                        size: Theme.avatarSizeSmall
                        isGroup: model.type === "group"
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2

                        Label {
                            Layout.fillWidth: true
                            text: model.displayName
                            font.pixelSize: Theme.fontSizeBody
                            font.weight: Font.DemiBold
                            color: Theme.textPrimary
                            elide: Text.ElideRight
                        }

                        Label {
                            text: model.type === "group"
                                  ? (model.memberCount + " 位成员") : "私聊"
                            font.pixelSize: Theme.fontSizeCaption
                            color: Theme.textTertiary
                        }
                    }

                    // 选中标记
                    Icon {
                        visible: model.conversationId === root.selectedConversationId
                        name: "check"
                        size: 14
                        iconColor: Theme.primaryColor
                    }
                }
            }

            // 空状态：没有任何会话（或快照为空）
            Column {
                anchors.centerIn: parent
                spacing: Theme.spacingMedium
                visible: targetModel.count === 0

                Icon {
                    anchors.horizontalCenter: parent.horizontalCenter
                    name: "forward"
                    size: 32
                    iconColor: Theme.textTertiary
                }

                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "暂无可转发的会话"
                    font.pixelSize: Theme.fontSizeMedium
                    color: Theme.textTertiary
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingSmall

            Label {
                Layout.fillWidth: true
                text: root.messageIds.length + " 条消息"
                font.pixelSize: Theme.fontSizeCaption
                color: Theme.textTertiary
            }

            AppButton {
                text: "取消"
                variant: "flat"
                onClicked: root.close()
            }

            AppButton {
                text: "转发"
                variant: "primary"
                enabled: root.selectedTarget !== null
                onClicked: root.confirmSelection()
            }
        }
    }

    function confirmSelection() {
        var target = selectedTarget
        if (target === null) {
            return
        }
        forwardConfirmed(messageIds, target.conversationId,
                         target.type === "group", target.peerUserId || 0)
        close()
    }
}
