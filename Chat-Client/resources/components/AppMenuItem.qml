import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "../theme"

// 主题化菜单项（配合 AppMenu 使用）。
//
// 为什么必须自绘：本 app 固定使用 Basic 样式（main.cpp 里
// QQuickStyle::setStyle("Basic")），而 Basic 的 MenuItem 其面板、文字与图标
// 颜色全部取自 control.palette（系统调色板），于是暗色主题下弹出菜单是一块
// 浅灰面板 + 深色文字，与全局割裂。更糟的是它的图标走 IconLabel 的
// icon.color（依赖 Qt 的 icon 着色），对本仓的黑色 SVG 图标无效——调用点把
// icon.color 设成 Theme.textPrimary（暗色下是白）后，白图标落在浅灰面板上
// 几乎看不见。因此这里把背景、文字与图标全部改由 Theme 决定，图标复用 Icon
// 组件（与全仓同一套着色口径，见 Icon.qml 里 MultiEffect 的说明）。
MenuItem {
    id: control

    // 图标名（对应 resources/icons/<name>.svg）。传名字而不是 source，才能走
    // Icon 组件的着色链路；留空则不占图标位
    property string iconName: ""
    // 危险操作（删除类）：图标与文字都用错误色，降低误点概率
    property bool danger: false

    readonly property color itemTextColor: !enabled ? Theme.textTertiary
                                          : (danger ? Theme.errorColor : Theme.textPrimary)
    readonly property color itemIconColor: !enabled ? Theme.textTertiary
                                          : (danger ? Theme.errorColor : Theme.textSecondary)

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)
    padding: Theme.spacingSmall

    contentItem: RowLayout {
        spacing: Theme.spacingSmall

        Icon {
            name: control.iconName
            size: 16
            iconColor: control.itemIconColor
            visible: control.iconName.length > 0
            Layout.alignment: Qt.AlignVCenter
        }

        Label {
            text: control.text
            color: control.itemTextColor
            font.pixelSize: Theme.fontSizeMedium
            elide: Text.ElideRight
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
        }
    }

    // 悬停/键盘高亮：Basic 用 palette.light（浅色），这里改用 Theme.hoverColor。
    // 内缩 2px 让高亮块与面板边缘有呼吸感；几何写法与 Basic 一致（显式 x/y/w/h），
    // 不用 anchors，避免与 Control 自身的布局相互冲突
    background: Rectangle {
        x: 2
        y: 2
        width: control.width - 4
        height: control.height - 4
        implicitWidth: 160
        implicitHeight: 30
        radius: Theme.radiusSmall - 2
        color: control.highlighted && control.enabled ? Theme.hoverColor : "transparent"
    }
}
