import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../theme"
import "../components"

AppDialog {
    id: root

    title: "设置"
    width: 420

    // 缓存用量（字节），由 FileTransferManager.cacheBytes() 提供
    property real cacheBytes: 0

    signal clearCacheRequested()

    onOpened: refreshCacheSize()

    function refreshCacheSize() {
        if (typeof fileTransfer !== "undefined") {
            cacheBytes = fileTransfer.cacheBytes()
        }
    }

    function formatBytes(bytes) {
        if (bytes < 1024) return bytes + " B"
        if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + " KB"
        if (bytes < 1024 * 1024 * 1024) return (bytes / (1024 * 1024)).toFixed(1) + " MB"
        return (bytes / (1024 * 1024 * 1024)).toFixed(2) + " GB"
    }

    contentItem: ColumnLayout {
        spacing: Theme.spacingLarge

        // 外观
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingSmall

            Label {
                text: "外观"
                font.pixelSize: Theme.fontSizeSubtitle
                font.weight: Font.DemiBold
                color: Theme.textPrimary
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacingMedium

                Label {
                    Layout.fillWidth: true
                    text: "暗色主题"
                    font.pixelSize: Theme.fontSizeBody
                    color: Theme.textPrimary
                }

                Switch {
                    checked: typeof appSettings !== "undefined" ? appSettings.darkMode : false
                    onToggled: {
                        if (typeof appSettings !== "undefined") {
                            appSettings.darkMode = checked
                        }
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: Theme.separatorColor
        }

        // 通知
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingSmall

            Label {
                text: "通知"
                font.pixelSize: Theme.fontSizeSubtitle
                font.weight: Font.DemiBold
                color: Theme.textPrimary
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacingMedium

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0

                    Label {
                        text: "桌面通知"
                        font.pixelSize: Theme.fontSizeBody
                        color: Theme.textPrimary
                    }

                    Label {
                        text: "收到新消息时弹出系统通知"
                        font.pixelSize: Theme.fontSizeCaption
                        color: Theme.textTertiary
                    }
                }

                Switch {
                    checked: typeof appSettings !== "undefined" ? appSettings.notificationsEnabled : true
                    onToggled: {
                        if (typeof appSettings !== "undefined") {
                            appSettings.notificationsEnabled = checked
                        }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacingMedium

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0

                    Label {
                        text: "消息预览"
                        font.pixelSize: Theme.fontSizeBody
                        color: Theme.textPrimary
                    }

                    Label {
                        text: "在通知中显示消息内容"
                        font.pixelSize: Theme.fontSizeCaption
                        color: Theme.textTertiary
                    }
                }

                Switch {
                    enabled: typeof appSettings !== "undefined" ? appSettings.notificationsEnabled : true
                    checked: typeof appSettings !== "undefined" ? appSettings.messagePreviewEnabled : true
                    onToggled: {
                        if (typeof appSettings !== "undefined") {
                            appSettings.messagePreviewEnabled = checked
                        }
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: Theme.separatorColor
        }

        // 存储
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingSmall

            Label {
                text: "存储"
                font.pixelSize: Theme.fontSizeSubtitle
                font.weight: Font.DemiBold
                color: Theme.textPrimary
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacingMedium

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0

                    Label {
                        text: "文件缓存"
                        font.pixelSize: Theme.fontSizeBody
                        color: Theme.textPrimary
                    }

                    Label {
                        text: "已用 " + root.formatBytes(root.cacheBytes)
                        font.pixelSize: Theme.fontSizeCaption
                        color: Theme.textTertiary
                    }
                }

                AppButton {
                    text: "清除缓存"
                    variant: "secondary"
                    onClicked: {
                        root.clearCacheRequested()
                        root.refreshCacheSize()
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: Theme.separatorColor
        }

        // 关于
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingSmall

            Label {
                text: "关于"
                font.pixelSize: Theme.fontSizeSubtitle
                font.weight: Font.DemiBold
                color: Theme.textPrimary
            }

            Label {
                text: "XYChat " + Qt.application.version
                font.pixelSize: Theme.fontSizeBody
                color: Theme.textPrimary
            }

            Label {
                text: "Qt 6 · C++20 · 端到端加密"
                font.pixelSize: Theme.fontSizeCaption
                color: Theme.textTertiary
            }
        }
    }
}
