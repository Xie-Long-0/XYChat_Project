import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../theme"
import "../components"

AppDialog {
    id: root

    property int conversationId: 0
    signal searchRequested(string query)
    signal inviteRequested(int conversationId, var userIds)

    title: "邀请成员"
    width: 360

    contentItem: ColumnLayout {
        spacing: Theme.spacingMedium

        AppTextField {
            id: inviteSearchField
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.inputHeight
            placeholderText: "搜索用户名..."
            onAccepted: root.doSearch()
        }

        ListView {
            id: inviteResultList
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, 200)
            clip: true
            visible: inviteResultModel.count > 0
            model: ListModel { id: inviteResultModel }

            delegate: Rectangle {
                width: inviteResultList.width
                height: 40
                color: inviteDelegateMouse.containsMouse ? Theme.hoverColor : "transparent"
                radius: Theme.radiusSmall

                MouseArea {
                    id: inviteDelegateMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: inviteResultModel.setProperty(index, "selected", !model.selected)
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spacingMedium
                    anchors.rightMargin: Theme.spacingMedium

                    Icon {
                        name: model.selected ? "checkbox-checked" : "checkbox-unchecked"
                        size: 16
                        iconColor: model.selected ? Theme.primaryColor : Theme.textTertiary
                    }

                    Label {
                        Layout.fillWidth: true
                        text: model.username + " (ID: " + model.userId + ")"
                        font.pixelSize: Theme.fontSizeBody
                        color: Theme.textPrimary
                        elide: Text.ElideRight
                    }
                }
            }
        }

        Label {
            id: inviteStatus
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
                text: "邀请"
                onClicked: root.doInvite()
            }
        }
    }

    function doSearch() {
        var query = inviteSearchField.text.trim()
        if (query.length > 0) {
            inviteStatus.text = "搜索中..."
            root.searchRequested(query)
        }
    }

    function showResults(users) {
        inviteResultModel.clear()
        if (users.length === 0) {
            inviteStatus.text = "未找到匹配的用户"
            return
        }
        inviteStatus.text = ""
        for (var i = 0; i < users.length; i++) {
            inviteResultModel.append({
                userId: users[i].userId || 0,
                username: users[i].username || "",
                selected: false
            })
        }
    }

    function doInvite() {
        var ids = []
        for (var i = 0; i < inviteResultModel.count; i++) {
            var u = inviteResultModel.get(i)
            if (u.selected) {
                ids.push(u.userId)
            }
        }
        if (ids.length === 0) {
            inviteStatus.text = "请先选择要邀请的用户"
            return
        }
        root.inviteRequested(root.conversationId, ids)
        close()
    }

    onClosed: {
        inviteSearchField.text = ""
        inviteResultModel.clear()
        inviteStatus.text = ""
    }
}
