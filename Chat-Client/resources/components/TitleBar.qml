import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QWindowKit

import "../theme"

Rectangle {
    id: titleBar
    height: Theme.titleBarHeight
    color: Theme.titleBarBackground

    property WindowAgent windowAgent
    property Window window
    property string title: ""
    property bool showCloseButton: true

    Component.onCompleted: windowAgent.setTitleBar(this)

    // 标题
    Text {
        anchors {
            verticalCenter: parent.verticalCenter
            left: parent.left
            leftMargin: 10
            right: toolButtonRow.left
            rightMargin: 10
        }
        verticalAlignment: Text.AlignVCenter
        text: titleBar.title
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSizeMedium
        font.weight: Font.DemiBold
        color: Theme.textPrimary
    }

    // 工具按钮栏
    Row {
        id: toolButtonRow
        anchors.right: captionButtonRow.left
        anchors.top: parent.top
        height: parent.height
        spacing: 2
        rightPadding: 6

        // M4.5: 亮/暗主题切换按钮
        Button {
            id: themeToggleBtn
            height: parent.height
            width: height * 1.5
            padding: 0
            background: Rectangle {
                color: {
                    if (!minimizeBtn.enabled)
                        return "gray";
                    if (themeToggleBtn.hovered)
                        return Theme.titleBarButtonHover
                    return "transparent"
                }
            }
            contentItem: Icon {
                name: Theme.darkMode ? "sun" : "moon"
                size: 16
                iconColor: Theme.textPrimary
            }

            onClicked: themeSettings.darkMode = !themeSettings.darkMode

            Component.onCompleted: windowAgent.setHitTestVisible(themeToggleBtn)
        }
    }

    // 窗口控制按钮
    Row {
        id: captionButtonRow
        anchors.right: parent.right
        anchors.top: parent.top
        height: parent.height
        spacing: 0
        visible: titleBar.showCloseButton

        // 最小化
        QWKButton {
            id: minimizeBtn
            height: parent.height
            source: "qrc:/icons/minimize.svg"
            background: Rectangle {
                color: {
                    if (!minimizeBtn.enabled)
                        return "gray";
                    if (minimizeBtn.hovered)
                        return Theme.titleBarButtonHover
                    return Theme.titleBarButtonBackground
                }
            }
            onClicked: window.showMinimized()
            Component.onCompleted: {
                titleBar.windowAgent.setSystemButton(WindowAgent.Minimize, minimizeBtn)
            }
        }

        // 最大化/还原
        QWKButton {
            id: maximizeBtn
            height: parent.height
            source: window.visibility === Window.Maximized ? "qrc:/icons/restore.svg" : "qrc:/icons/maximize.svg"
            background: Rectangle {
                color: {
                    if (!maximizeBtn.enabled)
                        return "gray";
                    if (maximizeBtn.hovered)
                        return Theme.titleBarButtonHover
                    return Theme.titleBarButtonBackground
                }
            }
            onClicked: {
                if (window.visibility === Window.Maximized) {
                    window.showNormal()
                } else {
                    window.showMaximized()
                }
            }
            Component.onCompleted: {
                windowAgent.setSystemButton(WindowAgent.Maximize, maximizeBtn)
            }
        }

        // 关闭
        QWKButton {
            id: closeBtn
            height: parent.height
            source: "qrc:/icons/close.svg"
            background: Rectangle {
                color: {
                    if (!closeBtn.enabled)
                        return "gray";
                    if (closeBtn.hovered)
                        return Theme.titleBarButtonCloseHover;
                    return Theme.titleBarButtonCloseBackground;
                }
            }
            onClicked: window.close()
            Component.onCompleted: {
                windowAgent.setSystemButton(WindowAgent.Close, closeBtn)
            }
        }
    }
}
