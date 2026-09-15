import QtQuick
import QtQuick.Controls

import "../theme"

// 主题化弹出菜单容器（与 AppMenuItem 配套使用）。
//
// Basic 样式的 Menu 面板底色取 control.palette.window（系统浅色）、边框取
// palette.dark，暗色主题下就是一块突兀的浅色方块，与全局割裂。这里连同圆角、
// 边框与内边距一起改由 Theme 决定，与 AppDialog 同一口径；菜单项样式见
// AppMenuItem。
Menu {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            contentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding)
    padding: Theme.spacingXSmall

    // 菜单宽度实际由这里的 implicitWidth 决定：Basic 的 Menu contentItem 是
    // ListView，它只声明 implicitHeight（内容的纵向总和），不声明 implicitWidth，
    // 因此"面板最小宽度"是菜单宽度的唯一来源；菜单项再被拉伸到这个宽度
    background: Rectangle {
        implicitWidth: 168
        implicitHeight: 34
        color: Theme.windowBackground
        border.width: 1
        border.color: Theme.borderColor
        radius: Theme.radiusMedium
    }
}
