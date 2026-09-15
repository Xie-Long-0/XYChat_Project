import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

import "../theme"

Rectangle {
    id: messageInput
    height: inputColumn.implicitHeight + Theme.spacingMedium * 2
    color: Theme.inputBackground

    signal messageSent(string text)
    signal attachmentSelected(var fileUrl)
    // P3.2: 用户正在输入（文本变化且非空时触发，供上层发“正在输入”信号）
    signal typingActivity()

    property int maxLength: 4096

    FileDialog {
        id: attachmentDialog
        title: qsTr("选择要发送的文件")
        fileMode: FileDialog.OpenFile
        onAccepted: {
            if (selectedFile.toString().length > 0) {
                attachmentSelected(selectedFile)
            }
        }
    }

    Rectangle {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 1
        color: Theme.separatorColor
    }

    ColumnLayout {
        id: inputColumn
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: Theme.spacingLarge
        anchors.rightMargin: Theme.spacingLarge
        spacing: Theme.spacingXSmall

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingSmall

            Rectangle {
                id: attachButton
                width: Theme.inputHeight
                height: Theme.inputHeight
                radius: Theme.inputHeight / 2
                color: attachMouse.pressed ? Theme.inputBorderColor : "transparent"
                opacity: typeof fileTransfer !== "undefined" && fileTransfer.enabled ? 1.0 : 0.4

                Behavior on color { ColorAnimation { duration: Theme.animationFast } }

                MouseArea {
                    id: attachMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        if (typeof fileTransfer !== "undefined" && fileTransfer.enabled) {
                            attachmentDialog.open()
                        }
                    }
                }

                Icon {
                    anchors.centerIn: parent
                    name: "attach"
                    size: 18
                    iconColor: Theme.textSecondary
                }
            }

            TextArea {
                id: inputField
                Layout.fillWidth: true
                Layout.minimumHeight: Theme.inputHeight
                Layout.maximumHeight: Theme.inputHeight * 5
                placeholderText: qsTr("输入消息...")
                font.pixelSize: Theme.fontSizeBody
                wrapMode: TextEdit.Wrap
                leftPadding: Theme.spacingMedium
                rightPadding: Theme.spacingMedium
                topPadding: Theme.spacingSmall
                bottomPadding: Theme.spacingSmall
                placeholderTextColor: Theme.inputPlaceholderColor
                color: Theme.textPrimary
                selectByMouse: true
                clip: true

                background: Rectangle {
                    radius: Theme.radiusMedium
                    color: Theme.chatBackground
                    border.width: inputField.activeFocus ? 2 : 1
                    border.color: inputField.activeFocus ? Theme.inputFocusBorderColor : Theme.inputBorderColor

                    Behavior on border.color { ColorAnimation { duration: Theme.animationFast } }
                }

                Keys.onReturnPressed: {
                    if (event.modifiers & Qt.ShiftModifier) {
                        inputField.insert(inputField.cursorPosition, "\n")
                    } else {
                        sendMessage()
                    }
                    event.accepted = true
                }

                Keys.onEnterPressed: {
                    if (event.modifiers & Qt.ShiftModifier) {
                        inputField.insert(inputField.cursorPosition, "\n")
                    } else {
                        sendMessage()
                    }
                    event.accepted = true
                }

                // P3.2: 键入时上报“正在输入”（C++ 侧节流，无需在此限频）
                onTextChanged: {
                    if (inputField.text.length > 0) {
                        messageInput.typingActivity()
                    }
                }
            }

            Rectangle {
                id: sendButton
                width: Theme.inputHeight
                height: Theme.inputHeight
                radius: Theme.inputHeight / 2
                color: sendMouse.pressed ? Theme.loginButtonPressed
                     : (sendMouse.containsMouse ? Theme.loginButtonHover : Theme.primaryColor)

                Behavior on color { ColorAnimation { duration: Theme.animationFast } }

                MouseArea {
                    id: sendMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: sendMessage()
                }

                Icon {
                    anchors.centerIn: parent
                    name: "send"
                    size: 18
                    iconColor: Theme.textOnPrimary
                }
            }
        }

        Label {
            Layout.alignment: Qt.AlignRight
            text: inputField.text.length + " / " + messageInput.maxLength
            font.pixelSize: Theme.fontSizeCaption
            color: inputField.text.length > messageInput.maxLength
                   ? Theme.errorColor : Theme.textTertiary
            visible: inputField.text.length > 0
        }
    }

    function sendMessage() {
        var text = inputField.text.trim()
        if (text.length > 0 && text.length <= messageInput.maxLength) {
            messageSent(text)
            inputField.text = ""
        }
    }

    function clearInput() {
        inputField.text = ""
    }

    function setFocus() {
        inputField.forceActiveFocus()
    }
}
