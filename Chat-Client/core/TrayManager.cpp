#include "TrayManager.h"

#include <QApplication>
#include <QIcon>
#include <QWindow>

TrayManager::TrayManager(QObject *parent)
    : QObject(parent)
{
}

TrayManager::~TrayManager()
{
    // QSystemTrayIcon 由 Qt 父对象机制管理，但 QMenu 无 parent 需显式释放
    delete m_trayMenu;
}

void TrayManager::setMainWindow(QWindow *window)
{
    m_mainWindow = window;
}

bool TrayManager::initialize()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        qWarning() << "[TrayManager] System tray not available";
        m_available = false;
        emit availableChanged();
        return false;
    }

    m_trayIcon = new QSystemTrayIcon(this);
    // 使用应用图标（chat-bubble.svg 经 QIcon 加载）
    m_trayIcon->setIcon(QIcon(":/icons/chat-bubble.svg"));
    m_trayIcon->setToolTip("XYChat");

    createTrayMenu();
    m_trayIcon->setContextMenu(m_trayMenu);

    connect(m_trayIcon, &QSystemTrayIcon::activated, this, &TrayManager::onTrayActivated);
    connect(m_trayIcon, &QSystemTrayIcon::messageClicked, this, &TrayManager::onTrayMessageClicked);

    m_trayIcon->show();
    m_available = true;
    emit availableChanged();
    return true;
}

void TrayManager::createTrayMenu()
{
    // QSystemTrayIcon::setContextMenu 不接管所有权，故在析构中显式释放
    m_trayMenu = new QMenu();

    m_openAction = m_trayMenu->addAction("打开 XYChat");
    connect(m_openAction, &QAction::triggered, this, [this]() {
        restoreFromTray();
    });

    m_trayMenu->addSeparator();

    m_quitAction = m_trayMenu->addAction("退出");
    connect(m_quitAction, &QAction::triggered, this, [this]() {
        emit quitRequested();
        QApplication::quit();
    });
}

void TrayManager::minimizeToTray()
{
    // 托盘不可用时不隐藏窗口（让 onClosing 走 Qt.quit() 分支）
    if (!m_mainWindow || !m_available) {
        return;
    }
    m_mainWindow->hide();
}

void TrayManager::restoreFromTray()
{
    if (!m_mainWindow) {
        return;
    }
    m_mainWindow->show();
    m_mainWindow->requestActivate();
}

void TrayManager::showNotification(qint64 conversationId, const QString &title, const QString &body)
{
    if (!m_trayIcon || !QSystemTrayIcon::supportsMessages()) {
        return;
    }
    // 抑制当前活动会话的通知（用户正在看该会话，无需弹窗打扰）
    if (conversationId == m_activeConversation) {
        return;
    }
    m_pendingNotificationConvId = conversationId;
    m_trayIcon->showMessage(title, body, QSystemTrayIcon::Information, 5000);
}

void TrayManager::setActiveConversation(qint64 convId)
{
    if (m_activeConversation == convId) {
        return;
    }
    m_activeConversation = convId;
    emit activeConversationChanged();
}

void TrayManager::onTrayActivated(QSystemTrayIcon::ActivationReason reason)
{
    switch (reason) {
    case QSystemTrayIcon::DoubleClick:
    case QSystemTrayIcon::Trigger:
        restoreFromTray();
        break;
    default:
        break;
    }
}

void TrayManager::onTrayMessageClicked()
{
    if (m_pendingNotificationConvId > 0) {
        emit notificationClicked(m_pendingNotificationConvId);
        m_pendingNotificationConvId = 0;
    }
    restoreFromTray();
}
