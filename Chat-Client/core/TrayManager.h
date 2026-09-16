#pragma once

#include <QObject>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QAction>

class QWindow;

// M11A: 系统托盘管理器
//
// 职责：
// - 持有 QSystemTrayIcon 并管理其生命周期
// - 提供最小化到托盘的方法（hide + 清除任务栏）
// - 托盘图标右键菜单（打开/退出）
// - 双击托盘图标恢复窗口
// - M11A A3: 桌面通知（showMessage）与点击回调
//
// 使用方式：
// - main.cpp 创建实例并注册为 QML context property "trayManager"
// - MainWindow.qml 的 onClosing 调用 minimizeToTray()
// - QML 绑定 activeConversation 属性以抑制当前会话的通知
class TrayManager : public QObject
{
    Q_OBJECT
    // 当前活动会话 ID（由 QML MainPage 绑定），用于抑制该会话的桌面通知
    Q_PROPERTY(qint64 activeConversation READ activeConversation WRITE setActiveConversation NOTIFY activeConversationChanged)
    // 托盘是否可用（系统支持且初始化成功）
    Q_PROPERTY(bool available READ isAvailable NOTIFY availableChanged)

public:
    explicit TrayManager(QObject *parent = nullptr);
    ~TrayManager() override;

    // 设置关联的主窗口（用于恢复/最小化）
    void setMainWindow(QWindow *window);

    // 初始化托盘图标（须在 QApplication 创建后调用）
    bool initialize();

    // 最小化到托盘：隐藏窗口并清除任务栏按钮
    Q_INVOKABLE void minimizeToTray();

    // 从托盘恢复窗口
    Q_INVOKABLE void restoreFromTray();

    // M11A A3: 显示桌面通知
    // conversationId: 关联的会话 ID（点击通知时回传）
    // title: 通知标题（发送者/群名）
    // body: 通知正文（消息预览）
    Q_INVOKABLE void showNotification(qint64 conversationId, const QString &title, const QString &body);

    qint64 activeConversation() const { return m_activeConversation; }
    void setActiveConversation(qint64 convId);
    bool isAvailable() const { return m_available; }

signals:
    void activeConversationChanged();
    void availableChanged();
    // M11A A3: 用户点击了桌面通知，携带关联的会话 ID
    void notificationClicked(qint64 conversationId);
    // 用户请求退出应用（托盘菜单"退出"）
    void quitRequested();

private slots:
    void onTrayActivated(QSystemTrayIcon::ActivationReason reason);
    void onTrayMessageClicked();

private:
    void createTrayMenu();

    QSystemTrayIcon *m_trayIcon = nullptr;
    QMenu *m_trayMenu = nullptr;
    QAction *m_openAction = nullptr;
    QAction *m_quitAction = nullptr;
    QWindow *m_mainWindow = nullptr;
    qint64 m_activeConversation = 0;
    qint64 m_pendingNotificationConvId = 0; // 最近一条通知关联的会话 ID
    bool m_available = false; // 托盘是否可用
};
