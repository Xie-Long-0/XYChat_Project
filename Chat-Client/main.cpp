#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>

#include <QWKQuick/qwkquickglobal.h>

#include "core/NetworkManager.h"
#include "core/FileImageProvider.h"
#include "core/FileTransferManager.h"
#include "core/MediaPlaybackManager.h"
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

    // M8.3b: 音视频播放器（C++ QMediaPlayer + DecryptingIODevice，从密文缓存
    // 流式解密播放，明文不落盘）。声明在 engine 之前，保证 engine 销毁时
    // context property 仍存活（栈对象按声明逆序销毁）
    XYChat::Client::MediaPlaybackManager mediaPlayback(
        static_cast<XYChat::Client::FileTransferManager *>(networkManager.fileTransfer()));

    // 创建 QML 引擎
    QQmlApplicationEngine engine;

    // 暴露 C++ 对象到 QML
    engine.rootContext()->setContextProperty("networkManager", &networkManager);
    engine.rootContext()->setContextProperty("themeSettings", &themeSettings);
    // M8.2: 文件传输引擎（数据面 HTTP + 分片加解密 + 密文缓存）单独暴露，
    // QML 直接连其进度/状态信号并调用上传/下载/保存；文件密钥与清单不经 QML
    engine.rootContext()->setContextProperty("fileTransfer", networkManager.fileTransfer());
    // M8.3b: 音视频播放器暴露给 QML（播放/暂停/进度/音量）
    engine.rootContext()->setContextProperty("mediaPlayer", &mediaPlayback);

    // M8.3: 应用内图片查看器的图像源（image://xyfile/<messageId>）：从密文缓存
    // 逐片解密后在内存中解码，明文不落盘。provider 的所有权交给引擎
    engine.addImageProvider(
        "xyfile",
        new XYChat::Client::FileImageProvider(
            static_cast<XYChat::Client::FileTransferManager *>(networkManager.fileTransfer())));

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
