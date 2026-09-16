#pragma once

#include <QObject>
#include <QSettings>

// M11A: 应用设置统一管理（QSettings 持久化），暴露给 QML 作为设置页的数据源。
// 合并原 ThemeSettings 的职责（darkMode），并新增通知/托盘偏好。
// 所有属性均带 NOTIFY 以支持 QML 响应式绑定。
class AppSettings : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool darkMode READ darkMode WRITE setDarkMode NOTIFY darkModeChanged)
    Q_PROPERTY(bool notificationsEnabled READ notificationsEnabled WRITE setNotificationsEnabled NOTIFY notificationsEnabledChanged)
    Q_PROPERTY(bool messagePreviewEnabled READ messagePreviewEnabled WRITE setMessagePreviewEnabled NOTIFY messagePreviewEnabledChanged)
    Q_PROPERTY(bool minimizeToTray READ minimizeToTray WRITE setMinimizeToTray NOTIFY minimizeToTrayChanged)

public:
    explicit AppSettings(QObject *parent = nullptr) : QObject(parent)
    {
        m_darkMode = m_settings.value("theme/darkMode", false).toBool();
        m_notificationsEnabled = m_settings.value("notifications/enabled", true).toBool();
        m_messagePreviewEnabled = m_settings.value("notifications/messagePreview", true).toBool();
        m_minimizeToTray = m_settings.value("tray/minimizeOnClose", true).toBool();
    }

    bool darkMode() const { return m_darkMode; }
    bool notificationsEnabled() const { return m_notificationsEnabled; }
    bool messagePreviewEnabled() const { return m_messagePreviewEnabled; }
    bool minimizeToTray() const { return m_minimizeToTray; }

    void setDarkMode(bool dark)
    {
        if (m_darkMode == dark) return;
        m_darkMode = dark;
        m_settings.setValue("theme/darkMode", dark);
        emit darkModeChanged();
    }

    void setNotificationsEnabled(bool enabled)
    {
        if (m_notificationsEnabled == enabled) return;
        m_notificationsEnabled = enabled;
        m_settings.setValue("notifications/enabled", enabled);
        emit notificationsEnabledChanged();
    }

    void setMessagePreviewEnabled(bool enabled)
    {
        if (m_messagePreviewEnabled == enabled) return;
        m_messagePreviewEnabled = enabled;
        m_settings.setValue("notifications/messagePreview", enabled);
        emit messagePreviewEnabledChanged();
    }

    void setMinimizeToTray(bool minimize)
    {
        if (m_minimizeToTray == minimize) return;
        m_minimizeToTray = minimize;
        m_settings.setValue("tray/minimizeOnClose", minimize);
        emit minimizeToTrayChanged();
    }

signals:
    void darkModeChanged();
    void notificationsEnabledChanged();
    void messagePreviewEnabledChanged();
    void minimizeToTrayChanged();

private:
    QSettings m_settings;
    bool m_darkMode = false;
    bool m_notificationsEnabled = true;
    bool m_messagePreviewEnabled = true;
    bool m_minimizeToTray = true;
};
