import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects

import "../theme"
import "../components"

Item {
    id: loginPage
    anchors.fill: parent

    signal loginRequested(string username, string password)
    signal registerRequested(string username, string password, string email, string phone)
    signal loginSucceeded()

    property bool isRegisterMode: false
    property string errorMessage: ""
    property string successMessage: ""
    property bool isLoading: false

    // 背景
    Rectangle {
        anchors.fill: parent
        color: Theme.loginBackground
    }

    // 居中卡片
    Rectangle {
        id: loginCard
        anchors.centerIn: parent
        width: 400
        height: cardLayout.implicitHeight + 60
        radius: Theme.radiusLarge
        color: Theme.windowBackground
        border.width: 1
        border.color: Theme.borderColor

        // 阴影效果（Qt 6 使用内置的 MultiEffect，DropShadow 属于 Qt5Compat.GraphicalEffects）
        layer.enabled: true
        layer.effect: MultiEffect {
            shadowEnabled: true
            shadowHorizontalOffset: 0
            shadowVerticalOffset: 4
            shadowBlur: 0.6
            shadowColor: Qt.rgba(0, 0, 0, 0.1)
        }

        ColumnLayout {
            id: cardLayout
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 30
            spacing: Theme.spacingMedium

            // Logo / 标题
            ColumnLayout {
                Layout.alignment: Qt.AlignHCenter
                spacing: Theme.spacingSmall

                // 圆形 Logo
                Rectangle {
                    Layout.alignment: Qt.AlignHCenter
                    width: 64; height: 64
                    radius: 32
                    color: Theme.primaryColor

                    Label {
                        anchors.centerIn: parent
                        text: "XY"
                        font.pixelSize: Theme.fontSizeXLarge
                        font.weight: Font.Bold
                        color: Theme.textOnPrimary
                    }
                }

                Label {
                    Layout.alignment: Qt.AlignHCenter
                    text: "XYChat"
                    font.pixelSize: Theme.fontSizeTitle
                    font.weight: Font.Bold
                    color: Theme.textPrimary
                }

                Label {
                    Layout.alignment: Qt.AlignHCenter
                    text: isRegisterMode ? "创建新账号" : "登录你的账号"
                    font.pixelSize: Theme.fontSizeMedium
                    color: Theme.textSecondary
                }
            }

            Item { Layout.preferredHeight: Theme.spacingSmall }

            // 用户名输入框
            TextField {
                id: usernameField
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.inputHeight
                placeholderText: "用户名"
                font.pixelSize: Theme.fontSizeMedium
                color: Theme.textPrimary
                placeholderTextColor: Theme.inputPlaceholderColor
                selectByMouse: true
                background: Rectangle {
                    radius: Theme.radiusSmall
                    color: Theme.inputBackground
                    border.width: usernameField.activeFocus ? 2 : 1
                    border.color: usernameField.activeFocus ? Theme.inputFocusBorderColor : Theme.inputBorderColor
                }
            }

            // 密码输入框
            TextField {
                id: passwordField
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.inputHeight
                placeholderText: "密码"
                echoMode: TextField.Password
                font.pixelSize: Theme.fontSizeMedium
                color: Theme.textPrimary
                placeholderTextColor: Theme.inputPlaceholderColor
                selectByMouse: true
                background: Rectangle {
                    radius: Theme.radiusSmall
                    color: Theme.inputBackground
                    border.width: passwordField.activeFocus ? 2 : 1
                    border.color: passwordField.activeFocus ? Theme.inputFocusBorderColor : Theme.inputBorderColor
                }
            }

            // 注册额外字段
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.spacingSmall
                visible: isRegisterMode
                opacity: isRegisterMode ? 1 : 0

                Behavior on opacity { NumberAnimation { duration: Theme.animationNormal } }

                TextField {
                    id: emailField
                    Layout.fillWidth: true
                    Layout.preferredHeight: Theme.inputHeight
                    placeholderText: "邮箱（可选）"
                    font.pixelSize: Theme.fontSizeMedium
                    color: Theme.textPrimary
                    placeholderTextColor: Theme.inputPlaceholderColor
                    selectByMouse: true
                    background: Rectangle {
                        radius: Theme.radiusSmall
                        color: Theme.inputBackground
                        border.width: emailField.activeFocus ? 2 : 1
                        border.color: emailField.activeFocus ? Theme.inputFocusBorderColor : Theme.inputBorderColor
                    }
                }

                TextField {
                    id: phoneField
                    Layout.fillWidth: true
                    Layout.preferredHeight: Theme.inputHeight
                    placeholderText: "手机号（可选）"
                    font.pixelSize: Theme.fontSizeMedium
                    color: Theme.textPrimary
                    placeholderTextColor: Theme.inputPlaceholderColor
                    selectByMouse: true
                    background: Rectangle {
                        radius: Theme.radiusSmall
                        color: Theme.inputBackground
                        border.width: phoneField.activeFocus ? 2 : 1
                        border.color: phoneField.activeFocus ? Theme.inputFocusBorderColor : Theme.inputBorderColor
                    }
                }
            }

            // 错误信息
            Label {
                Layout.fillWidth: true
                text: errorMessage
                color: Theme.loginErrorColor
                font.pixelSize: Theme.fontSizeSmall
                visible: errorMessage !== ""
                horizontalAlignment: Text.AlignHCenter
            }

            // 成功提示
            Label {
                Layout.fillWidth: true
                text: successMessage
                color: Theme.successColor
                font.pixelSize: Theme.fontSizeSmall
                visible: successMessage !== ""
                horizontalAlignment: Text.AlignHCenter
            }

            // 主按钮
            Button {
                id: mainButton
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.inputHeight
                text: isRegisterMode ? "注 册" : "登 录"
                enabled: !isLoading && usernameField.text.length > 0 && passwordField.text.length > 0
                font.pixelSize: Theme.fontSizeLarge
                font.weight: Font.DemiBold

                background: Rectangle {
                    radius: Theme.radiusSmall
                    color: mainButton.enabled
                        ? (mainButton.pressed ? Theme.loginButtonPressed
                            : (mainButton.hovered ? Theme.loginButtonHover : Theme.loginButtonColor))
                        : Theme.textTertiary

                    Behavior on color { ColorAnimation { duration: Theme.animationFast } }
                }

                contentItem: Item {
                    Label {
                        anchors.centerIn: parent
                        text: mainButton.text
                        font: mainButton.font
                        color: Theme.textOnPrimary
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        visible: !loginPage.isLoading
                    }
                    LoadingIndicator {
                        anchors.centerIn: parent
                        size: 20
                        running: loginPage.isLoading
                        visible: loginPage.isLoading
                        iconColor: Theme.textOnPrimary
                    }
                }

                onClicked: {
                    errorMessage = ""
                    successMessage = ""
                    if (isRegisterMode) {
                        registerRequested(usernameField.text, passwordField.text,
                                          emailField.text, phoneField.text)
                    } else {
                        loginRequested(usernameField.text, passwordField.text)
                    }
                }
            }

            // 切换按钮
            Button {
                id: switchButton
                Layout.alignment: Qt.AlignHCenter
                text: isRegisterMode ? "已有账号？返回登录" : "没有账号？注册新账号"
                flat: true
                font.pixelSize: Theme.fontSizeSmall

                contentItem: Label {
                    text: switchButton.text
                    font: switchButton.font
                    color: Theme.textLink
                    horizontalAlignment: Text.AlignHCenter
                }

                background: null

                onClicked: {
                    isRegisterMode = !isRegisterMode
                    errorMessage = ""
                    successMessage = ""
                }
            }
        }
    }

    // 公共方法
    function showError(msg) {
        errorMessage = msg
        successMessage = ""
        isLoading = false
    }

    function showSuccess(msg) {
        successMessage = msg
        errorMessage = ""
        isLoading = false
        isRegisterMode = false
    }

    function onLoginSuccess() {
        isLoading = false
        loginSucceeded()
    }

    function setLoading(loading) {
        isLoading = loading
    }
}
