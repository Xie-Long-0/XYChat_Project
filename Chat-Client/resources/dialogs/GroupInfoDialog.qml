import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../theme"
import "../components"

AppDialog {
    id: root

    property int convId: 0
    property string myRole: "member"
    property int myUserId: 0

    signal inviteClicked()
    signal leaveRequested(int conversationId)
    signal kickRequested(int conversationId, int userId)
    signal refreshRequested(int conversationId)

    title: "群信息"
    width: 400

    contentItem: ColumnLayout {
        spacing: Theme.spacingMedium

        Label {
            id: groupInfoTitleText
            font.pixelSize: Theme.fontSizeSubtitle
            font.weight: Font.DemiBold
            color: Theme.textPrimary
            elide: Text.ElideRight
            Layout.fillWidth: true
        }

        Label {
            id: groupInfoSubtitle
            font.pixelSize: Theme.fontSizeCaption
            color: Theme.textSecondary
        }

        ListView {
            id: memberList
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, 260)
            clip: true
            model: ListModel { id: memberModel }

            delegate: Rectangle {
                width: memberList.width
                height: 44
                color: memberDelegateMouse.containsMouse ? Theme.hoverColor : "transparent"
                radius: Theme.radiusSmall

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

        RowLayout {
            Layout.fillWidth: true
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
                onClicked: {
                    root.leaveRequested(root.convId)
                    root.close()
                }
            }
        }
    }

    function showInfo(info) {
        convId = info.conversationId || 0
        myRole = info.myRole || "member"
        groupInfoTitleText.text = info.name || "未命名群组"
        groupInfoSubtitle.text = (info.memberCount || 0) + " 位成员 · 你是" + roleDisplay(myRole)
        memberModel.clear()
        var members = info.members || []
        for (var i = 0; i < members.length; i++) {
            memberModel.append({
                userId: members[i].userId || 0,
                username: members[i].username || "",
                role: members[i].role || "member"
            })
        }
        if (!visible) {
            open()
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
}
