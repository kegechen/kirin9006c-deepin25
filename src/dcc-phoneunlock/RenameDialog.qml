// 设置手机的别名：只管这台电脑上显示什么，手机配对时上报的名字保留不动。
// 清空并保存 = 不用别名，恢复显示手机的名字。不用重新配对。
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.deepin.dtk 1.0 as D

D.DialogWindow {
    id: dialog
    property string phoneId
    property string phoneName   // 现在显示的名（别名优先，没有就是手机上报名）

    width: 400
    minimumWidth: width
    maximumWidth: width
    modality: Qt.WindowModal
    title: qsTr("设置别名")

    function save() {
        var n = edit.text.trim()
        if (n === dialog.phoneName) {
            dialog.close()
            return
        }
        var e = dccData.rename(dialog.phoneId, n)
        if (e)
            err.text = e
        else
            dialog.close()
    }

    ColumnLayout {
        width: dialog.width - 20
        spacing: 8

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            opacity: 0.7
            text: qsTr("别名只在这台电脑上显示（锁屏和手机列表），手机原来的名字不变；清空保存就恢复用手机的名字")
        }
        D.LineEdit {
            id: edit
            Layout.fillWidth: true
            text: dialog.phoneName
            onTextEdited: {
                var n = dccData.utf8Bytes(text.trim())
                err.text = n > dccData.nameMaxBytes ? qsTr("太长了（%1/%2 字节，一个汉字算 3 字节）").arg(n).arg(dccData.nameMaxBytes) : ""
            }
            onAccepted: dialog.save()
            Component.onCompleted: {
                forceActiveFocus()
                selectAll()
            }
        }
        Label {
            id: err
            Layout.fillWidth: true
            visible: text !== ""
            wrapMode: Text.WordWrap
            color: "#e8533c"
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 10
            Layout.bottomMargin: 10
            Button {
                Layout.fillWidth: true
                text: qsTr("取消")
                onClicked: dialog.close()
            }
            D.RecommandButton {
                Layout.fillWidth: true
                text: qsTr("保存")
                enabled: dccData.utf8Bytes(edit.text.trim()) <= dccData.nameMaxBytes
                onClicked: dialog.save()
            }
        }
    }
}
