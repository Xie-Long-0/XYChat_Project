import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

import "../theme"

Rectangle {
    id: messageInput
    height: inputRow.implicitHeight + Theme.spacingMedium * 2
    color: Theme.inputBackground

    signal messageSent(string text)
    // M8.2: 用户选定附件后上报本地路径（上传与清单封装全在 C++ 侧完成，
    // 文件密钥不经 QML）
    signal attachmentSelected(string filePath)

    // M8.2: 附件选择器。fileMode 为 OpenFile（单选），路径以本地文件形式传给引擎
    FileDialog {
        id: attachmentDialog
        title: qsTr("选择要发送的文件")
        fileMode: FileDialog.OpenFile
        onAccepted: {
            if (selectedFile.toString().length > 0) {
                // 用 Qt.urlToLocalFile 而不是正则剔 file:// 前缀：后者对 UNC
                // 路径与含 %/#/? 的文件名会给出错误结果，上传时表现为"文件不存在"
                attachmentSelected(Qt.urlToLocalFile(selectedFile))
            }
        }
    }

    // 顶部分隔线
    Rectangle {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 1
        color: Theme.separatorColor
    }

    RowLayout {
        id: inputRow
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: Theme.spacingLarge
        anchors.rightMargin: Theme.spacingLarge
        spacing: Theme.spacingSmall

        // M8.2: 附件按钮（回形针）。未开启文件能力时置灰并提示，
        // 而不是让用户点了没反应
        Rectangle {
            id: attachButton
            width: Theme.inputHeight
            height: Theme.inputHeight
            radius: Theme.inputHeight / 2
            color: attachMouse.pressed ? Theme.inputBorderColor : "transparent"
            // context property 未注册时直接引用会抛 ReferenceError，故用 typeof 判定
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

            Canvas {
                anchors.centerIn: parent
                width: 18; height: 18
                onPaint: {
                    var ctx = getContext("2d")
                    ctx.clearRect(0, 0, width, height)
                    ctx.strokeStyle = Theme.textSecondary
                    ctx.lineWidth = 2
                    ctx.lineCap = "round"
                    // 回形针：两段同心圆弧加一条斜线
                    ctx.beginPath()
                    ctx.moveTo(5, 12)
                    ctx.lineTo(12, 5)
                    ctx.stroke()
                    ctx.beginPath()
                    ctx.arc(5.5, 12.5, 2.5, Math.PI * 0.5, Math.PI * 2)
                    ctx.stroke()
                    ctx.beginPath()
                    ctx.arc(12.5, 5.5, 3.5, Math.PI * 1.2, Math.PI * 2.7)
                    ctx.stroke()
                }
            }
        }

        // 输入框
        TextField {
            id: inputField
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.inputHeight
            placeholderText: qsTr("输入消息...")
            font.pixelSize: Theme.fontSizeMedium
            wrapMode: TextEdit.Wrap
            leftPadding: Theme.spacingMedium

            placeholderTextColor: Theme.inputPlaceholderColor
            color: Theme.textPrimary
            selectByMouse: true

            background: Rectangle {
                radius: Theme.radiusMedium
                color: Theme.chatBackground
                border.width: inputField.activeFocus ? 2 : 1
                border.color: inputField.activeFocus ? Theme.inputFocusBorderColor : Theme.inputBorderColor

                Behavior on border.color { ColorAnimation { duration: Theme.animationFast } }
            }

            Keys.onReturnPressed: {
                if (text.trim().length > 0) {
                    sendMessage()
                }
            }

            Keys.onEnterPressed: {
                if (text.trim().length > 0) {
                    sendMessage()
                }
            }
        }

        // 发送按钮
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

            // 发送箭头图标
            Canvas {
                anchors.centerIn: parent
                width: 18; height: 18
                onPaint: {
                    var ctx = getContext("2d")
                    ctx.clearRect(0, 0, width, height)
                    ctx.fillStyle = Theme.textOnPrimary
                    ctx.beginPath()
                    ctx.moveTo(2, width / 2)
                    ctx.lineTo(width - 2, width / 2)
                    ctx.lineTo(width - 6, 3)
                    ctx.closePath()
                    ctx.fill()
                    ctx.beginPath()
                    ctx.moveTo(2, width / 2)
                    ctx.lineTo(width - 2, width / 2)
                    ctx.lineTo(width - 6, width - 3)
                    ctx.closePath()
                    ctx.fill()
                }
            }
        }
    }

    function sendMessage() {
        var text = inputField.text.trim()
        if (text.length > 0) {
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
