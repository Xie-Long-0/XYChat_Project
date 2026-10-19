import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../theme"

// M11C C1: 群信息内容面板。抽屉（GroupInfoDrawer）与 C3 宽屏常驻右栏共用
// 此面板，本身不带弹出/滑出行为，只负责展示与操作入口
Item {
    id: root

    property int convId: 0
    property string myRole: "member"
    property int myUserId: 0
    // C3 常驻右栏时隐藏关闭按钮（收起由布局层负责）
    property bool showCloseButton: true

    signal closeRequested()
    signal inviteClicked()
    signal leaveRequested(int conversationId)
    signal kickRequested(int conversationId, int userId)
    signal refreshRequested(int conversationId)

    implicitWidth: Theme.rightPanelWidth

    function showInfo(info) {
        convId = info.conversationId || 0
        myRole = info.myRole || "member"
        groupTitleText.text = info.name || "未命名群组"
        groupSubtitle.text = (info.memberCount || 0) + " 位成员 · 你是" + roleDisplay(myRole)
        memberModel.clear()
        var members = info.members || []
        for (var i = 0; i < members.length; i++) {
            memberModel.append({
                userId: members[i].userId || 0,
                username: members[i].username || "",
                role: members[i].role || "member"
            })
        }
    }

    function roleDisplay(role) {
        if (role === "owner") return "群主"
        if (role === "admin") return "管理员"
        return "成员"
    }

    function canKick(memberRole) {
        if (myRole === "owner") return memberRole !== "owner"
        if (myRole === "admin") return memberRole === "member"
        return false
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: Theme.spacingMedium
            spacing: Theme.spacingSmall

            Label {
                text: "群信息"
                font.pixelSize: Theme.fontSizeHeading
                font.weight: Font.DemiBold
                color: Theme.textPrimary
                Layout.fillWidth: true
            }

            Rectangle {
                visible: root.showCloseButton
                width: 32
                height: 32
                radius: 16
                color: closeMouse.containsMouse ? Theme.hoverColor : "transparent"

                Icon {
                    anchors.centerIn: parent
                    name: "close"
                    size: 14
                    iconColor: Theme.textSecondary
                }

                MouseArea {
                    id: closeMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.closeRequested()
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            height: 1
            color: Theme.dividerColor
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.margins: Theme.spacingMedium
            spacing: Theme.spacingXSmall

            Label {
                id: groupTitleText
                font.pixelSize: Theme.fontSizeSubtitle
                font.weight: Font.DemiBold
                color: Theme.textPrimary
                elide: Text.ElideRight
                Layout.fillWidth: true
            }

            Label {
                id: groupSubtitle
                font.pixelSize: Theme.fontSizeCaption
                color: Theme.textSecondary
            }
        }

        ListView {
            id: memberList
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.topMargin: Theme.spacingSmall
            clip: true
            model: ListModel { id: memberModel }

            delegate: Rectangle {
                width: memberList.width
                height: 44
                radius: Theme.radiusSmall
                color: memberDelegateMouse.containsMouse ? Theme.hoverColor : "transparent"

                MouseArea {
                    id: memberDelegateMouse
                    anchors.fill: parent
                    hoverEnabled: true
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spacingMedium
                    anchors.rightMargin: Theme.spacingMedium

                    Avatar {
                        userId: model.userId
                        name: model.username
                        size: 28
                    }

                    Label {
                        Layout.fillWidth: true
                        text: model.username + (model.userId == root.myUserId ? "（我）" : "")
                        font.pixelSize: Theme.fontSizeBody
                        color: Theme.textPrimary
                        elide: Text.ElideRight
                    }

                    Label {
                        text: root.roleDisplay(model.role)
                        font.pixelSize: Theme.fontSizeCaption
                        color: model.role === "owner" ? Theme.primaryColor : Theme.textTertiary
                    }

                    AppButton {
                        visible: root.canKick(model.role) && model.userId !== root.myUserId
                        text: "移除"
                        variant: "flat"
                        font.pixelSize: Theme.fontSizeCaption
                        onClicked: {
                            root.kickRequested(root.convId, model.userId)
                            root.refreshRequested(root.convId)
                        }
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            height: 1
            color: Theme.dividerColor
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: Theme.spacingMedium
            spacing: Theme.spacingSmall

            AppButton {
                text: "邀请成员"
                variant: "flat"
                onClicked: root.inviteClicked()
            }

            Item { Layout.fillWidth: true }

            AppButton {
                text: "退出群聊"
                variant: "flat"
                onClicked: root.leaveRequested(root.convId)
            }
        }
    }
}
