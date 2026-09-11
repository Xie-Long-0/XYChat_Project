#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>

#include <QWKQuick/qwkquickglobal.h>

#include "core/NetworkManager.h"
#include "core/ThemeSettings.h"

int main(int argc, char *argv[])
{
    QGuiApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    //QQuickWindow::setDefaultAlphaBuffer(true);

    QGuiApplication app(argc, argv);
    app.setOrganizationName("XYChat");
    app.setApplicationName("XYChat");

    // 设置默认样式
    QQuickStyle::setStyle("Basic");

    // 创建 NetworkManager
    NetworkManager networkManager;

    // M4.5: 主题偏好持久化
    ThemeSettings themeSettings;

    // 创建 QML 引擎
    QQmlApplicationEngine engine;

    // 暴露 C++ 对象到 QML
    engine.rootContext()->setContextProperty("networkManager", &networkManager);
    engine.rootContext()->setContextProperty("themeSettings", &themeSettings);
    // M8.2: 文件传输引擎（数据面 HTTP + 分片加解密 + 密文缓存）单独暴露，
    // QML 直接连其进度/状态信号并调用上传/下载/保存；文件密钥与清单不经 QML
    engine.rootContext()->setContextProperty("fileTransfer", networkManager.fileTransfer());

    // 注册 QWindowKit QML 类型
    QWK::registerTypes(&engine);

    // 加载 QML
    engine.addImportPath(":/");
    engine.load(QUrl("qrc:/main.qml"));

    if (engine.rootObjects().isEmpty()) {
        return -1;
    }

    // M4.5 修复：主窗口作为独立根窗口加载（不再声明在 main.qml 内），
    // 避免被当作登录窗口的 transient 子窗口而不在系统任务栏显示
    engine.load(QUrl("qrc:/pages/MainWindow.qml"));

    QObject *loginRoot = nullptr;
    QObject *mainWindow = nullptr;
    for (QObject *obj : engine.rootObjects()) {
        if (obj->objectName() == QLatin1String("mainWindow")) {
            mainWindow = obj;
        } else if (obj->objectName() == QLatin1String("loginRoot")) {
            loginRoot = obj;
        }
    }
    if (mainWindow != nullptr && loginRoot != nullptr) {
        loginRoot->setProperty("mainWindow", QVariant::fromValue(mainWindow));
    } else {
        qCritical() << "[Main] Failed to locate login/main root windows"
                    << "loginRoot:" << loginRoot << "mainWindow:" << mainWindow;
        return -1;
    }

    return app.exec();
}
