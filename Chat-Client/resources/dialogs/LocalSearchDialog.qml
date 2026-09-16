import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../theme"
import "../components"

AppDialog {
    id: root

    title: "搜索消息"
    width: 480
    height: 520

    signal jumpToMessageRequested(int conversationId, int messageId)

    property var searchResults: []

    onOpened: {
        searchField.text = ""
        searchResults = []
        searchField.forceActiveFocus()
    }

    contentItem: ColumnLayout {
        spacing: Theme.spacingMedium

        // 搜索输入框
        AppTextField {
            id: searchField
            Layout.fillWidth: true
            placeholderText: "输入关键词搜索本地消息..."
            onAccepted: root.doSearch(searchField.text)
        }

        AppButton {
            Layout.alignment: Qt.AlignRight
            text: "搜索"
            variant: "primary"
            onClicked: root.doSearch(searchField.text)
        }

        // 结果列表
        ListView {
            id: resultList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: ListModel { id: resultModel }

            delegate: Rectangle {
                width: resultList.width
                height: 64
                color: resultMouse.containsMouse ? Theme.hoverColor : "transparent"
                radius: Theme.radiusSmall

                MouseArea {
                    id: resultMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        root.jumpToMessageRequested(model.conversationId, model.messageId)
                        root.close()
                    }
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spacingMedium
                    anchors.rightMargin: Theme.spacingMedium
                    spacing: Theme.spacingSmall

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacingSmall

                            Label {
                                text: model.conversationName
                                font.pixelSize: Theme.fontSizeBody
                                font.weight: Font.DemiBold
                                color: Theme.textPrimary
                                elide: Text.ElideRight
                                Layout.maximumWidth: 160
                            }

                            Label {
                                text: model.senderUsername
                                font.pixelSize: Theme.fontSizeCaption
                                color: Theme.textTertiary
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }

                            Label {
                                text: model.timeStr
                                font.pixelSize: Theme.fontSizeCaption
                                color: Theme.textTertiary
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            text: model.snippet
                            font.pixelSize: Theme.fontSizeSmall
                            color: Theme.textSecondary
                            elide: Text.ElideRight
                            maximumLineCount: 2
                        }
                    }
                }

                Rectangle {
                    anchors.bottom: parent.bottom
                    anchors.left: parent.left
                    anchors.right: parent.right
                    height: 1
                    color: Theme.dividerColor
                }
            }

            // 空状态
            Column {
                anchors.centerIn: parent
                spacing: Theme.spacingMedium
                visible: resultModel.count === 0 && searchResults.length === 0

                Icon {
                    anchors.horizontalCenter: parent.horizontalCenter
                    name: "search"
                    size: 32
                    iconColor: Theme.textTertiary
                }

                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "输入关键词搜索本地缓存的消息"
                    font.pixelSize: Theme.fontSizeMedium
                    color: Theme.textTertiary
                }
            }
        }

        // 结果计数
        Label {
            Layout.fillWidth: true
            text: searchResults.length > 0 ? "找到 " + searchResults.length + " 条匹配消息" : ""
            font.pixelSize: Theme.fontSizeCaption
            color: Theme.textTertiary
            visible: searchResults.length > 0
        }
    }

    function doSearch(query) {
        if (query.length === 0) {
            searchResults = []
            resultModel.clear()
            return
        }
        if (typeof networkManager === "undefined") {
            return
        }
        var results = networkManager.searchMessages(query, 50, 0)
        searchResults = results
        resultModel.clear()
        for (var i = 0; i < results.length; i++) {
            var msg = results[i]
            var convId = msg.conversationId || 0
            var convName = msg.conversationName || ("会话 " + convId)
            var content = msg.content || ""
            var snippet = content.length > 80 ? content.substring(0, 80) + "..." : content
            resultModel.append({
                messageId: msg.messageId || 0,
                conversationId: convId,
                conversationName: convName,
                senderUsername: msg.senderUsername || "",
                snippet: snippet,
                timeStr: root.formatTime(msg.createdAt || "")
            })
        }
    }

    function formatTime(isoStr) {
        if (!isoStr || isoStr.length === 0) return ""
        var d = new Date(isoStr)
        if (isNaN(d.getTime())) return ""
        var now = new Date()
        if (d.toDateString() === now.toDateString()) {
            var hh = d.getHours()
            var mm = d.getMinutes()
            return (hh < 10 ? "0" : "") + hh + ":" + (mm < 10 ? "0" : "") + mm
        }
        return (d.getMonth() + 1) + "/" + d.getDate()
    }
}
