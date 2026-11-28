// 删除（解除绑定）一台手机：电脑上删掉它的公钥，它就再也解锁不了这台电脑。
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.deepin.dtk 1.0 as D

D.DialogWindow {
    id: dialog
    property string phoneId
    property string phoneName

    width: 400
    minimumWidth: width
    maximumWidth: width
    modality: Qt.WindowModal

    ColumnLayout {
        width: dialog.width - 20
        spacing: 8

        Label {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            font: D.DTK.fontManager.t5
            text: qsTr("删除手机“%1”？").arg(dialog.phoneName)
        }
        Label {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            opacity: 0.7
            text: qsTr("删除后这台手机就不能再解锁电脑，要再用得重新配对。手机 App 里的这台电脑也可以一并删掉。")
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
            Layout.topMargin: 20
            Layout.bottomMargin: 10
            Button {
                Layout.fillWidth: true
                text: qsTr("取消")
                onClicked: dialog.close()
            }
            D.WarningButton {
                Layout.fillWidth: true
                text: qsTr("删除")
                onClicked: {
                    var e = dccData.unpair(dialog.phoneId)
                    if (e)
                        err.text = e
                    else
                        dialog.close()
                }
            }
        }
    }
}
