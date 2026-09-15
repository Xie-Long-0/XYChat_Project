import QtQuick
import QtQuick.Effects
import "../theme"

Image {
    id: root

    property string name: ""
    property real size: 16
    property color iconColor: Theme.textSecondary

    width: size
    height: size
    source: name.length > 0 ? "qrc:/icons/" + name + ".svg" : ""
    fillMode: Image.PreserveAspectFit
    smooth: true
    mipmap: true
    cache: true

    layer.enabled: true
    layer.effect: MultiEffect {
        // 关键：MultiEffect.colorization 是「乘法着色」而不是「替换颜色」。
        // 见 qtdeclarative/src/effects/data/shaders/multieffect.frag：
        //     color.rgb = gray * colorizationColor.rgb * colorizationColor.a
        //                 + color.rgb * (1.0 - colorizationColor.a)
        // 其中 gray 是源图的灰度。图标源文件绝大多数以黑色描边（gray == 0），
        // 乘任何 colorizationColor 结果仍是黑色——这正是暗色主题下图标
        // （搜索/加号/刷新/发送箭头/加载指示器等）全部变黑的原因。
        //
        // 同文件第 82 行在 colorization 之前先执行 color.rgb += brightness * color.a，
        // 即 brightness 先于着色生效。因此这里先用 brightness: 1.0 把图标
        // 抬成白色剪影（gray == 1），再着色即可得到与 iconColor 完全一致的颜色；
        // 且乘上了 alpha，透明区域不受影响，抗锯齿边缘仍保持正确过渡。
        brightness: 1.0
        colorizationColor: root.iconColor
        colorization: 1.0
        autoPaddingEnabled: false
    }
}
