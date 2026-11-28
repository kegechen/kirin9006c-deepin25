// 添加手机：显示配对二维码（和 sudo hisi-phone pair 的一样），等手机扫码。dccData.pairState：1 = 等认证，2 = 二维码有效。
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.deepin.dtk 1.0 as D

D.DialogWindow {
    id: dialog
    property string result: ""       // 配对结束的原因（见 PhoneUnlock::pairFinished），"" = 还在进行
    property string phoneName: ""
    property string failure: ""
    property int secondsLeft: 0

    width: 420
    minimumWidth: width
    maximumWidth: width
    modality: Qt.WindowModal
    title: qsTr("添加手机")

    function resultTitle() {
        switch (dialog.result) {
        case "paired": return qsTr("已绑定“%1”").arg(dialog.phoneName)
        case "timeout": return qsTr("二维码过期了")
        case "replaced": return qsTr("这次配对被别处新开的配对顶掉了")
        case "bad_code": return qsTr("校验失败太多次，二维码已作废")
        case "service_gone": return qsTr("手机解锁服务退出了")
        case "error": return qsTr("没能开始配对")
        default: return qsTr("配对已取消")
        }
    }

    function resultDetail() {
        switch (dialog.result) {
        case "paired": return qsTr("锁屏后在手机上打开“电脑解锁”，点这台电脑、按指纹就能解锁。")
        case "error": return dialog.failure
        case "bad_code": return qsTr("手机扫的可能不是这次的二维码。")
        default: return ""
        }
    }

    // stopPair 由 phoneunlockMain.qml 里 Loader 的 onClosing 调用（实例级处理器优先于这里的）
    Component.onDestruction: dccData.stopPair()

    // DialogWindow 的默认 content 只收 Item；Connections / Timer 得显式放到 Window 的 data 里
    data: [
        Connections {
            target: dccData
            function onPairFinished(result, name) {
                dialog.result = result
                dialog.phoneName = name
            }
            function onPairFailed(msg) {
                dialog.failure = msg
                dialog.result = "error"
            }
            function onPairChanged() {
                if (dccData.pairState === 2) {
                    dialog.result = ""
                    copyButton.copied = false
                    qr.requestPaint()
                }
            }
        },
        Timer {
            interval: 500
            repeat: true
            triggeredOnStart: true
            running: dccData.pairState === 2
            onTriggered: dialog.secondsLeft = Math.max(0, Math.round((dccData.pairDeadline - Date.now()) / 1000))
        }
    ]

    ColumnLayout {
        width: dialog.width - 20
        spacing: 8

        // 二维码
        ColumnLayout {
            Layout.fillWidth: true
            visible: dccData.pairState === 2
            spacing: 8

            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: qsTr("在手机上打开“电脑解锁”App →“配对新电脑”，扫下面的二维码")
            }
            Rectangle {
                Layout.alignment: Qt.AlignHCenter
                implicitWidth: 272
                implicitHeight: 272
                radius: 8
                color: "white"
                Canvas {
                    id: qr
                    anchors.fill: parent
                    anchors.margins: 14
                    property string bits: dccData.qrBits
                    property int n: dccData.qrWidth
                    onBitsChanged: requestPaint()
                    onPaint: {
                        var ctx = getContext("2d")
                        ctx.reset()
                        ctx.fillStyle = "#ffffff"
                        ctx.fillRect(0, 0, width, height)
                        if (n <= 0)
                            return
                        var cell = Math.floor(Math.min(width, height) / n)
                        var off = Math.floor((Math.min(width, height) - cell * n) / 2)
                        ctx.fillStyle = "#000000"
                        for (var y = 0; y < n; y++)
                            for (var x = 0; x < n; x++)
                                if (bits.charCodeAt(y * n + x) === 49)
                                    ctx.fillRect(off + x * cell, off + y * cell, cell, cell)
                    }
                }
                Label {
                    anchors.centerIn: parent
                    width: parent.width - 40
                    visible: dccData.qrWidth === 0
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    color: "black"
                    text: qsTr("显示不了二维码（没装 libqrencode4）。请复制配对链接发到手机，在 App 里“粘贴配对链接”")
                }
            }
            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("%1 秒内有效，用一次就作废").arg(dialog.secondsLeft)
            }
            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                font: D.DTK.fontManager.t8
                opacity: 0.6
                text: qsTr("电脑：%1　IP：%2　蓝牙：%3").arg(dccData.pairHost)
                      .arg(dccData.pairIps !== "" ? dccData.pairIps.split(",").join("、") : qsTr("无"))
                      .arg(dccData.pairBt !== "" ? dccData.pairBt : qsTr("没开"))
            }
            Label {
                Layout.fillWidth: true
                visible: dccData.pairIps === "" && dccData.pairBt === ""
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                color: "#e8533c"
                text: qsTr("电脑没联网也没开蓝牙，手机连不上")
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 10
                Layout.bottomMargin: 10
                Button {
                    id: copyButton
                    property bool copied: false
                    Layout.fillWidth: true
                    text: copied ? qsTr("已复制（别发给别人）") : qsTr("复制配对链接")
                    onClicked: {
                        dccData.copyPairLink()
                        copied = true
                    }
                }
                Button {
                    Layout.fillWidth: true
                    text: qsTr("取消")
                    onClicked: dialog.close()
                }
            }
        }

        // 等认证（点了“重新生成二维码”）
        Label {
            Layout.fillWidth: true
            Layout.topMargin: 30
            Layout.bottomMargin: 30
            visible: dccData.pairState === 1
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: qsTr("请在弹出的认证框里输入你的登录密码……")
        }

        // 结果
        ColumnLayout {
            Layout.fillWidth: true
            visible: dccData.pairState === 0
            spacing: 8

            Label {
                Layout.fillWidth: true
                Layout.topMargin: 20
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                font: D.DTK.fontManager.t5
                text: dialog.resultTitle()
            }
            Label {
                Layout.fillWidth: true
                visible: text !== ""
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                opacity: 0.7
                text: dialog.resultDetail()
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 20
                Layout.bottomMargin: 10
                Button {
                    Layout.fillWidth: true
                    text: dialog.result === "paired" ? qsTr("完成") : qsTr("关闭")
                    onClicked: dialog.close()
                }
                D.RecommandButton {
                    Layout.fillWidth: true
                    visible: dialog.result !== "paired" && dialog.result !== "service_gone"
                    enabled: dccData.serviceOk && dccData.phones.length < dccData.maxPhones
                    text: qsTr("重新生成二维码")
                    onClicked: dccData.startPair()
                }
            }
        }
    }
}
