import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QWindowKit

import "theme"
import "pages"
import "components"

ApplicationWindow {
    id: root
    objectName: "loginRoot"
    width: 480
    height: 640
    minimumWidth: 420
    minimumHeight: 600
    visible: false
    color: "transparent"
    title: "XYChat"

    // M4.5 修复：主窗口改为独立根窗口（由 main.cpp 加载后注入），
    // 不再是登录窗口的声明式子窗口，避免被当作 transient 子窗口而不在任务栏显示
    property var mainWindow

    // QWindowKit WindowAgent
    WindowAgent {
        id: windowAgent
    }

    // M11A: 主题模式绑定（Theme 为全局单例，绑定一次即可作用于所有窗口）
    Binding {
        target: Theme
        property: "darkMode"
        value: typeof appSettings !== "undefined" ? appSettings.darkMode : false
    }

    Component.onCompleted: {
        windowAgent.setup(root)
        root.visible = true
    }

    // 关闭登录窗口：若主窗口已打开则仅隐藏登录窗口，否则退出应用
    // （主窗口作为独立根窗口由 main.cpp 加载，关闭主窗口才退出）
    onClosing: {
        if (mainWindow !== undefined && mainWindow !== null && mainWindow.visible) {
            root.hide()
            close.accepted = false
        } else {
            Qt.quit()
        }
    }

    // 主布局
    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // 自定义标题栏
        TitleBar {
            id: titleBar
            Layout.fillWidth: true
            window: root
            windowAgent: windowAgent
            title: "XYChat"
        }

        // 登录页面
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            LoginPage {
                id: loginPage

                onLoginRequested: function(username, password) {
                    loginPage.setLoading(true)
                    networkManager.login(username, password)
                }

                onRegisterRequested: function(username, password, email, phone) {
                    loginPage.setLoading(true)
                    networkManager.registerAccount(username, password, email, phone)
                }
            }
        }
    }

    // 主窗口登出信号（mainWindow 为 main.cpp 注入的独立根窗口；
    // 初始为 undefined，延迟到注入后再创建 Connections，避免初始化期 QML 报错）
    Loader {
        active: mainWindow !== undefined && mainWindow !== null
        sourceComponent: Connections {
            target: mainWindow

            function onLogoutRequested() {
                networkManager.logout()
                mainWindow.hide()
                loginPage.setLoading(false)
                root.show()
            }
        }
    }

    // NetworkManager 认证信号连接
    Connections {
        target: networkManager

        function onLoginSuccessful() {
            loginPage.onLoginSuccess()
            if (mainWindow !== undefined && mainWindow !== null) {
                mainWindow.myUserId = networkManager.userId
                mainWindow.myUsername = networkManager.username
                root.hide()
                mainWindow.show()
                mainWindow.loadConversations()
            }
        }

        function onLoginFailed(errorMessage) {
            loginPage.showError(errorMessage)
        }

        // P2: 会话失效（过期/被终止/续期被拒）——回登录页并提示重新登录
        function onSessionExpired() {
            if (mainWindow !== undefined && mainWindow !== null) {
                mainWindow.resetUi()
                mainWindow.hide()
            }
            loginPage.setLoading(false)
            loginPage.showError("会话已过期，请重新登录")
            root.show()
        }

        function onRegisterSuccessful() {
            loginPage.showSuccess("注册成功，请登录")
        }

        function onRegisterFailed(errorMessage) {
            loginPage.showError(errorMessage)
            loginPage.setLoading(false)
        }
    }
}
