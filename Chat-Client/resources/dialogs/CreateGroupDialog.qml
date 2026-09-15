import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../theme"
import "../components"

AppDialog {
    id: root

    signal createRequested(string name, var memberIds)
    signal loadContactsRequested()

    title: "新建群组"
    width: 380

    onOpened: root.loadContactsRequested()

    contentItem: ColumnLayout {
        spacing: Theme.spacingMedium

        AppTextField {
            id: groupNameField
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.inputHeight
            placeholderText: "群名称（1-64 字符）"
        }

        Label {
            text: "选择初始成员（联系人）"
            font.pixelSize: Theme.fontSizeCaption
            color: Theme.textSecondary
            visible: contactModel.count > 0
        }

        ListView {
            id: contactList
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, 220)
            clip: true
            visible: contactModel.count > 0
            model: ListModel { id: contactModel }

            delegate: Rectangle {
                width: contactList.width
                height: 40
                color: contactDelegateMouse.containsMouse ? Theme.hoverColor : "transparent"
                radius: Theme.radiusSmall

                MouseArea {
                    id: contactDelegateMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: contactModel.setProperty(index, "selected", !model.selected)
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
            text: "暂无联系人，可先搜索添加联系人"
            font.pixelSize: Theme.fontSizeCaption
            color: Theme.textTertiary
            visible: contactModel.count === 0
        }

        Label {
            id: createGroupStatus
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
                text: "创建"
                onClicked: root.doCreate()
            }
        }
    }

    function showContacts(contacts) {
        contactModel.clear()
        for (var i = 0; i < contacts.length; i++) {
            contactModel.append({
                userId: contacts[i].userId || 0,
                username: contacts[i].username || "",
                selected: false
            })
        }
    }

    function doCreate() {
        var name = groupNameField.text.trim()
        if (name.length === 0 || name.length > 64) {
            createGroupStatus.text = "群名称需为 1-64 字符"
            return
        }
        var ids = []
        for (var i = 0; i < contactModel.count; i++) {
            var c = contactModel.get(i)
            if (c.selected) {
                ids.push(c.userId)
            }
        }
        root.createRequested(name, ids)
        close()
    }

    onClosed: {
        groupNameField.text = ""
        contactModel.clear()
        createGroupStatus.text = ""
    }
}
