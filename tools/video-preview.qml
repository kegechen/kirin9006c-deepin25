// 模拟控制中心壁纸页顶部的动态壁纸预览：QtMultimedia MediaPlayer + VideoOutput，循环播放
import QtQuick
import QtQuick.Window
import QtMultimedia

Window {
    width: 480; height: 270; visible: true
    title: "video preview test"
    MediaPlayer {
        id: player
        source: Qt.resolvedUrl(Qt.application.arguments[Qt.application.arguments.length - 1])
        videoOutput: out
        loops: MediaPlayer.Infinite
        Component.onCompleted: play()
    }
    VideoOutput {
        id: out
        anchors.fill: parent
        fillMode: VideoOutput.PreserveAspectCrop
    }
}
