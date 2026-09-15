import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../theme"
import "../components"

AppDialog {
    id: root

    signal searchRequested(string query)
    signal openChatRequested(int userId, string username)
    signal addContactRequested(int userId)

    title: "搜索用户"
    width: 360

    onOpened: searchField.forceActiveFocus()

    contentItem: ColumnLayout {
        spacing: Theme.spacingMedium

        Label {
            text: "输入用户名搜索"
            font.pixelSize: Theme.fontSizeBody
            color: Theme.textPrimary
        }

        AppTextField {
            id: searchField
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.inputHeight
            placeholderText: "用户名..."
            onAccepted: root.doSearch()
            Keys.onDownPressed: {
                if (searchResultModel.count > 0) {
                    searchResults.currentIndex = 0
                    searchResults.forceActiveFocus()
                }
            }
        }

        ListView {
            id: searchResults
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, 200)
            clip: true
            visible: searchResultModel.count > 0
            model: ListModel { id: searchResultModel }
            keyNavigationEnabled: true
            highlight: Rectangle {
                color: Theme.hoverColor
                radius: Theme.radiusSmall
            }
            highlightFollowsCurrentItem: true

            Keys.onReturnPressed: {
                if (currentIndex >= 0 && currentIndex < searchResultModel.count) {
                    var item = searchResultModel.get(currentIndex)
                    root.openChatRequested(item.userId, item.username)
                    root.addContactRequested(item.userId)
                    root.close()
                }
            }
            Keys.onEscapePressed: root.close()

            delegate: Rectangle {
                width: searchResults.width
                height: 44
                color: searchDelegateMouse.containsMouse ? Theme.hoverColor : "transparent"
                radius: Theme.radiusSmall

                MouseArea {
                    id: searchDelegateMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        root.openChatRequested(model.userId, model.username)
                        root.addContactRequested(model.userId)
                        root.close()
                    }
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spacingMedium
                    anchors.rightMargin: Theme.spacingMedium

                    Avatar {
                        userId: model.userId
                        name: model.username
                        size: 32
                    }

                    Label {
                        Layout.fillWidth: true
                        text: model.username + " (ID: " + model.userId + ")"
                        font.pixelSize: Theme.fontSizeBody
                        color: Theme.textPrimary
                    }
                }
            }
        }

        EmptyState {
            Layout.fillWidth: true
            visible: !searchLoading.running && searchStatus.text === "未找到匹配的用户"
            iconName: "search"
            title: "未找到匹配的用户"
            subtitle: "尝试其他关键词"
        }

        Row {
            id: searchStatusRow
            spacing: Theme.spacingSmall
            visible: searchLoading.running || (searchStatus.text !== "" && searchStatus.text !== "未找到匹配的用户")

            LoadingIndicator {
                id: searchLoading
                size: 14
                running: false
                visible: running
                anchors.verticalCenter: parent.verticalCenter
            }

            Label {
                id: searchStatus
                text: ""
                font.pixelSize: Theme.fontSizeCaption
                color: Theme.textSecondary
                anchors.verticalCenter: parent.verticalCenter
            }
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
                text: "搜索"
                onClicked: root.doSearch()
            }
        }
    }

    function doSearch() {
        var query = searchField.text.trim()
        if (query.length > 0) {
            searchLoading.running = true
            searchStatus.text = "搜索中..."
            root.searchRequested(query)
        }
    }

    function showResults(users) {
        searchLoading.running = false
        searchResultModel.clear()
        if (users.length === 0) {
            searchStatus.text = "未找到匹配的用户"
            return
        }
        searchStatus.text = ""
        for (var i = 0; i < users.length; i++) {
            var u = users[i]
            searchResultModel.append({
                userId: u.userId || 0,
                username: u.username || ""
            })
        }
    }

    onClosed: {
        searchField.text = ""
        searchResultModel.clear()
        searchStatus.text = ""
        searchLoading.running = false
    }
}
