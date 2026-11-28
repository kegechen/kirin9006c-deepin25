// “账户 → 登录方式 → 手机解锁”页面。dccData 是 phoneunlock.so 里的 PhoneUnlock（经 org.hisi.BioUnlock1.User 找 hisi-bio-unlock）。
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.deepin.dcc 1.0
import org.deepin.dtk 1.0 as D
import org.deepin.dtk.style 1.0 as DS

DccObject {
    id: root
    readonly property string base: "phoneunlock"
    readonly property int count: dccData.phones.length
    readonly property bool full: count >= dccData.maxPhones
    property string message: ""          // 最近一次操作的错误，显示在页面顶上
    // 别名/删除对话框的目标（从列表按钮传出来；对话框放在页面级，列表刷新时不会被一起销毁）
    property string renameId: ""
    property string renameName: ""
    property string deleteId: ""
    property string deleteName: ""
    property bool pairDismissed: false   // 用户主动关掉二维码窗口：别被状态绑定又弹出来

    Component.onCompleted: dccData.refresh()

    Connections {
        target: dccData
        function onPairFailed(msg) {
            root.message = qsTr("没能开始配对：%1").arg(msg)
        }
    }

    // 服务不可用 / 操作出错
    DccObject {
        name: "notice"
        parentName: root.base
        weight: 5
        visible: !dccData.serviceOk || root.message !== ""
        pageType: DccObject.Item
        page: RowLayout {
            Label {
                Layout.fillWidth: true
                Layout.margins: 10
                wrapMode: Text.WordWrap
                color: "#e8533c"
                text: !dccData.serviceOk ? qsTr("手机解锁暂时用不了：%1").arg(dccData.serviceError) : root.message
            }
            D.ToolButton {
                visible: root.message !== ""
                text: qsTr("知道了")
                onClicked: root.message = ""
            }
        }
    }

    DccTitleObject {
        name: "phonesTitle"
        parentName: root.base
        weight: 10
        displayName: qsTr("已绑定的手机")
        description: qsTr("最多 %1 台。手机装“电脑解锁”App 扫码配对后，锁屏时在手机上按指纹就能解锁这台电脑").arg(dccData.maxPhones)
    }

    DccObject {
        name: "phones"
        parentName: root.base
        weight: 20
        pageType: DccObject.Item
        page: DccGroupView {}

        DccObject {
            name: "empty"
            parentName: root.base + "/phones"
            weight: 10
            visible: root.count === 0
            pageType: DccObject.Item
            page: Label {
                leftPadding: 14
                topPadding: 12
                bottomPadding: 12
                opacity: 0.6
                text: dccData.serviceOk ? qsTr("还没有绑定手机") : qsTr("读不到已绑定的手机")
            }
        }

        DccRepeater {
            model: dccData.phones
            delegate: DccObject {
                id: phoneObj
                property var phone: modelData
                name: "phone_" + modelData.id
                parentName: root.base + "/phones"
                weight: 20 + index
                displayName: modelData.name
                description: (modelData.online ? qsTr("已连接") : qsTr("未连接")) + "　·　" + qsTr("%1 配对").arg(modelData.paired)
                pageType: DccObject.Editor
                page: RowLayout {
                    spacing: 0
                    D.ToolButton {
                        text: qsTr("别名")
                        onClicked: {
                            root.renameId = modelData.id
                            root.renameName = modelData.name
                            renameLoader.active = true
                        }
                    }
                    D.ToolButton {
                        text: qsTr("删除")
                        onClicked: {
                            root.deleteId = modelData.id
                            root.deleteName = modelData.name
                            deleteLoader.active = true
                        }
                    }
                }
            }
        }

        DccObject {
            name: "add"
            parentName: root.base + "/phones"
            weight: 100
            pageType: DccObject.Item
            page: RowLayout {
                D.ToolButton {
                    implicitHeight: DS.Style.itemDelegate.height
                    Layout.leftMargin: 5
                    enabled: dccData.serviceOk && dccData.pairState === 0 && !root.full
                    textColor: D.Palette {
                        normal {
                            common: D.DTK.makeColor(D.Color.Highlight)
                        }
                        normalDark: normal
                        hovered {
                            common: D.DTK.makeColor(D.Color.Highlight).lightness(+30)
                        }
                        hoveredDark: hovered
                    }
                    font: D.DTK.fontManager.t6
                    background: Item {}
                    text: dccData.pairState === 1 ? qsTr("请在认证框里输入密码……") : qsTr("添加手机…")
                    onClicked: {
                        root.message = ""
                        dccData.startPair()
                    }
                }
                Label {
                    Layout.fillWidth: true
                    visible: root.full
                    opacity: 0.6
                    wrapMode: Text.WordWrap
                    text: qsTr("已绑定 %1 台（上限）。换新手机要先删掉一台").arg(root.count)
                }
                Item {
                    Layout.fillWidth: !root.full
                }
                Loader {
                    id: renameLoader
                    active: false
                    sourceComponent: RenameDialog {
                        phoneId: root.renameId
                        phoneName: root.renameName
                        // 别在 closing 里同步删窗口（Loader 会 delete 它，close() 还在用）——推到下一轮
                        onClosing: Qt.callLater(function() { renameLoader.active = false })
                    }
                    onLoaded: item.show()
                }
                Loader {
                    id: deleteLoader
                    active: false
                    sourceComponent: DeleteDialog {
                        phoneId: root.deleteId
                        phoneName: root.deleteName
                        onClosing: Qt.callLater(function() { deleteLoader.active = false })
                    }
                    onLoaded: item.show()
                }
                Loader {
                    id: pairLoader
                    // 状态驱动：从别的页面回来（pairChanged 信号已经发过了）也能把二维码窗口补出来
                    active: dccData.pairState === 2 && !root.pairDismissed
                    sourceComponent: PairDialog {
                        onClosing: {
                            root.pairDismissed = true
                            Qt.callLater(function() { dccData.stopPair() })
                        }
                    }
                    onLoaded: item.show()
                }
                Connections {
                    target: dccData
                    function onPairChanged() {
                        if (dccData.pairState === 1 || dccData.pairState === 2)
                            root.pairDismissed = false
                    }
                }
            }
        }
    }

    DccTitleObject {
        name: "tryTitle"
        parentName: root.base
        weight: 30
        displayName: qsTr("试一试")
    }

    DccObject {
        name: "tryGroup"
        parentName: root.base
        weight: 40
        pageType: DccObject.Item
        page: DccGroupView {}

        DccObject {
            name: "lock"
            parentName: root.base + "/tryGroup"
            weight: 10
            displayName: qsTr("锁屏试试")
            description: qsTr("锁屏后在手机上打开“电脑解锁”，点这台电脑、按指纹就能解锁")
            pageType: DccObject.Editor
            page: Button {
                text: qsTr("锁屏")
                enabled: dccData.serviceOk && root.count > 0
                onClicked: {
                    var e = dccData.lockNow()
                    root.message = e ? qsTr("锁屏失败：%1").arg(e) : ""
                }
            }
        }
    }

    DccTitleObject {
        name: "helpTitle"
        parentName: root.base
        weight: 50
        displayName: qsTr("说明")
    }

    DccObject {
        name: "help"
        parentName: root.base
        weight: 60
        pageType: DccObject.Item
        backgroundType: DccObject.Normal
        page: Label {
            leftPadding: 14
            rightPadding: 14
            topPadding: 10
            bottomPadding: 10
            wrapMode: Text.WordWrap
            lineHeight: 1.3
            opacity: 0.75
            text: qsTr("· 手机装“电脑解锁”App，扫码配对；锁屏时在手机上按指纹解锁。\n"
                       + "· 配对要验证你的登录密码，二维码 3 分钟内有效、用一次就作废。\n"
                       + "· 手机丢了请马上在这里删掉它。")
        }
    }
}
