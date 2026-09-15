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
        colorizationColor: root.iconColor
        colorization: 1.0
        autoPaddingEnabled: false
    }
}
