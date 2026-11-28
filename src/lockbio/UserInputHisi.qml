// Copyright (C) 2023-2026 UnionTech Software Technology Co., Ltd.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
//
// kirin9006c-deepin25：treeland 0.11.0 src/plugins/lockscreen/qml/UserInput.qml 加上指纹 / 人脸入口。
// 由 libtl-lockbio.so 放进锁屏插件的资源目录（见 src/tl-lockbio.cpp），改动都标了“指纹 / 人脸”。
// 状态来自 hisi-bio-unlock（HisiBio.BioUnlock），认证和解锁也由它完成；这里只显示、以及点人脸图标时让它开摄像头。
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Shapes
import org.deepin.dtk 1.0 as D
import Treeland
import LockScreen
import HisiBio

Item {
    id: loginGroup
    width: 220
    height: 300

    property string normalHint: bioHint.length > 0 ? bioHint : qsTr("Please enter password")
    property bool enteringOtherUser: false
    property bool showUserNotFoundError: false
    property bool noPasswdLogin: false

    /* 指纹 / 人脸：只在锁屏、且是 hisi-bio-unlock 正在认证的那个用户时显示
     * 指纹 0 无  1 等待按压  2 刚才不匹配  3 已锁定  4 通过
     * 人脸 0 无  1 空闲      2 识别中      3 未通过  4 已锁定  5 通过 */
    readonly property bool bioActive: GreeterProxy.isLocked && !enteringOtherUser
                                      && BioUnlock.user.length > 0
                                      && BioUnlock.user === UserModel.currentUserName
    readonly property int fpState: bioActive ? BioUnlock.fingerprint : 0
    readonly property int faceState: bioActive ? BioUnlock.face : 0
    readonly property string bioHint: {
        const fp = fpState, face = faceState
        if (fp === 0 && face === 0)
            return ""
        if (fp === 4 || face === 5)
            return "验证成功"
        if (face === 2)
            return fp === 1 ? "正在识别人脸，也可以按压指纹" : "正在识别人脸…"
        if (fp === 2)
            return "指纹不匹配，请重试"
        const fpOk = fp === 1, faceOk = face === 1 || face === 3
        if (face === 3)
            return fpOk ? "人脸未通过，请按压指纹或点击人脸重试" : "人脸未通过，点击人脸图标重试"
        if (fpOk && faceOk)
            return "请按压指纹、点击人脸或输入密码"
        if (fpOk)
            return "请按压指纹或输入密码"
        if (faceOk)
            return "点击人脸图标识别，或输入密码"
        return "失败次数过多，请输入密码"
    }
    onNormalHintChanged: if (!enteringOtherUser) resetHint()

    /**************/
    /* Components */
    /**************/

    Item {
        width: 32
        height: 32
        visible: GreeterProxy.isLocked
        anchors {
            horizontalCenter: parent.horizontalCenter
            bottom: parent.top
            bottomMargin: 56
        }
        D.DciIcon {
            name: "login_lock"
            anchors.centerIn: parent
            sourceSize {
                width: 18
                height: 22
            }
        }
    }

    Column {
        id: userCol
        spacing: 15
        anchors.centerIn: parent

        Rectangle {
            width: 120
            height: 120
            anchors.horizontalCenter: parent.horizontalCenter
            color: "transparent"
            radius: 20
            border {
                width: 2
                color: Qt.rgba(1, 1, 1, 0.1)
            }

            D.QtIcon {
                id: avatar
                anchors.fill: parent
                visible: false
            }

            Rectangle {
                id: maskSource
                radius: 20
                anchors.fill: avatar
                visible: false
            }
            D.OpacityMask {
                anchors.fill: avatar
                source: avatar
                maskSource: maskSource
                invert: false
                visible: !loginGroup.enteringOtherUser
            }
            D.DciIcon {
                anchors.centerIn: parent
                name: "login_user"
                sourceSize { width: 96; height: 96 }
                visible: loginGroup.enteringOtherUser
            }
            Rectangle {
                radius: 20
                anchors.fill: avatar
                color: "transparent"
                border {
                    width: 1
                    color: Qt.rgba(0, 0, 0, 0.2)
                }
            }
        }

        Text {
            text: "User"
            id: username
            font.bold: true
            font.pixelSize: D.DTK.fontManager.t2.pixelSize
            font.family: D.DTK.fontManager.t2.family
            color: palette.windowText
            anchors.horizontalCenter: parent.horizontalCenter
        }

        Item {
            id: passwordSlot
            width: loginGroup.width
            height: 30
            anchors.horizontalCenter: parent.horizontalCenter

            TextField {
                id: passwordField

                property bool capsIndicatorVisible: false

                visible: !loginGroup.noPasswdLogin

                anchors.fill: parent

                cursorDelegate: Rectangle {
                    id: cursor

                    width: 1
                    height: 18
                    color: palette.windowText

                    visible: parent.activeFocus && !parent.readOnly && parent.selectionStart === parent.selectionEnd

                    Connections {
                        target: cursor.parent
                        function onCursorPositionChanged() {
                            // keep a moving cursor visible
                            cursor.opacity = 1
                            cursorTimer.restart()
                        }
                    }

                    Timer {
                        id: cursorTimer
                        running: cursor.parent.activeFocus && !cursor.parent.readOnly && interval != 0
                        repeat: true
                        // TODO: Application.styleHints.cursorFlashTime / 2, waylib is not supports
                        // Application.styleHints now.
                        interval: 600
                        onTriggered: cursor.opacity = !cursor.opacity ? 1 : 0
                        // force the cursor visible when gaining focus
                        onRunningChanged: cursor.opacity = 1
                    }
                }

                horizontalAlignment: TextInput.AlignHCenter
                echoMode: loginGroup.enteringOtherUser ? TextInput.Normal
                          : (showPasswordBtn.hiddenPWD ? TextInput.Password : TextInput.Normal)
                rightPadding: 22
                leftPadding: {
                    var remaining = loginGroup.width - contentWidth - rightPadding
                    if (capsIndicator.visible)
                        return rightPadding
                    return Math.max(8, remaining > rightPadding ? rightPadding : remaining)
                }
                maximumLength: 510
                placeholderText: loginGroup.enteringOtherUser ? qsTr("Username") : qsTr("Password")
                placeholderTextColor: Qt.rgba(1.0, 1.0, 1.0, 0.6)
                color: palette.windowText
                font: D.DTK.fontManager.t8
                Keys.onPressed: function (event) {
                    if (event.key === Qt.Key_CapsLock) {
                        capsIndicatorVisible = !capsIndicatorVisible
                        event.accepted = true
                    } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                        if (loginGroup.enteringOtherUser)
                            confirmOtherUser()
                        else
                            userLogin()
                        event.accepted = true
                    }
                }

                D.ActionButton {
                    id: capsIndicator
                    height: parent.height
                    anchors {
                        left: parent.left
                        leftMargin: 3
                        verticalCenter: parent.verticalCenter
                    }
                    visible: passwordField.capsIndicatorVisible && !loginGroup.enteringOtherUser
                    palette.windowText: undefined
                    icon {
                        name: "login_capslock"
                        height: 10
                        width: 10
                    }
                    Layout.alignment: Qt.AlignHCenter
                    implicitWidth: 16
                    implicitHeight: 16
                }

                D.ActionButton {
                    id: showPasswordBtn
                    anchors {
                        right: parent.right
                        rightMargin: 3
                        verticalCenter: parent.verticalCenter
                    }
                    property bool hiddenPWD: true
                    visible: !loginGroup.enteringOtherUser
                    icon {
                        name: hiddenPWD ? "login_display_password" : "login_hidden_password"
                        height: 10
                        width: 10
                    }
                    Layout.alignment: Qt.AlignHCenter
                    implicitWidth: 16
                    implicitHeight: 16
                    hoverEnabled: true

                    background: Rectangle {
                        anchors.fill: parent
                        radius: 4
                        color: showPasswordBtn.hovered ? Qt.rgba(
                                                            0, 0, 0,
                                                            0.1) : "transparent"
                    }

                    onClicked: hiddenPWD = !hiddenPWD
                }

                background: RoundBlur {
                    color: Qt.rgba(1, 1, 1, 0.4)
                    radius: 6
                }
            }

            D.ActionButton {
                id: noPwdLoginBtn
                visible: loginGroup.noPasswdLogin
                anchors.fill: parent
                icon {
                    name: "login_open"
                    width: 16
                    height: 16
                }
                background: RoundBlur {
                    anchors.fill: parent
                    color: Qt.rgba(1.0, 1.0, 1.0, 0.4)
                    radius: parent.height / 2
                }

                onClicked: userLogin()
            }
        }
    }

    D.ActionButton {
        id: loginBtn
        visible: !loginGroup.noPasswdLogin
        icon {
            name: "login_open"
            width: 16
            height: 16
        }
        height: passwordField.height
        width: height
        anchors {
            left: userCol.right
            bottom: userCol.bottom
            leftMargin: 20
        }
        enabled: passwordField.length != 0
        font: D.DTK.fontManager.t8
        background: RoundBlur {
            anchors.fill: parent
            color: Qt.rgba(1.0, 1.0, 1.0, 0.4)
            radius: parent.height / 2
        }

        onClicked: loginGroup.enteringOtherUser ? confirmOtherUser() : userLogin()
    }

    RowLayout {
        spacing: 10
        anchors {
            right: userCol.left
            bottom: userCol.bottom
            rightMargin: 10
        }
        height: passwordField.height

        RoundButton {
            id: langBtn
            text: 'L' // TODO: replace with icon
            visible: false
            Layout.fillHeight: true
            Layout.preferredWidth: passwordField.height
            background: Rectangle {
                anchors.fill: parent
                color: Qt.rgba(1.0, 1.0, 1.0, 0.4)
                radius: parent.height / 2
            }

            onClicked: {
                // TODO: impl language switch
                console.log("need impl langBtn")
            }
        }

        RoundButton {
            id: hintBtn
            icon {
                name: "login_hint"
                width: 10
                height: 10
            }
            visible: hintLabel.hintText.length != 0
            Layout.preferredWidth: 16
            Layout.preferredHeight: 16
            background: Rectangle {
                id: hintBtnBackground
                visible: hintBtn.hovered
                anchors.fill: parent
                color: Qt.rgba(1.0, 1.0, 1.0, 0.1)
                radius: 4
            }

            HintLabel {
                id: hintLabel
                x: hintBtn.width - hintLabel.width
                y: hintBtn.height + 11
                hintText: {
                    let user = UserModel.get(UserModel.currentUserName)
                    return user.passwordHint
                }
            }

            onClicked: hintLabel.open()
        }
    }

    Text {
        id: hintText
        visible: !loginGroup.enteringOtherUser || loginGroup.showUserNotFoundError
        font: D.DTK.fontManager.t8
        color: Qt.rgba(1.0, 1.0, 1.0, 0.7)
        anchors {
            horizontalCenter: parent.horizontalCenter
            top: userCol.bottom
            topMargin: 16
        }
    }

    /* 指纹 / 人脸图标：指纹一直在等，按上去就验证；人脸在有键盘 / 触控板动作时自动开，点图标也能开 */
    Row {
        id: bioRow
        spacing: 28
        visible: loginGroup.fpState !== 0 || loginGroup.faceState !== 0
        anchors {
            horizontalCenter: parent.horizontalCenter
            top: hintText.bottom
            topMargin: 20
        }

        BioButton {
            visible: loginGroup.fpState !== 0
            // 指纹纹路（参照 Lucide 图标 fingerprint，ISC 许可）
            glyph: "M12 10a2 2 0 0 0-2 2c0 1.02-.1 2.51-.26 4 M14 13.12c0 2.38 0 6.38-1 8.88"
                   + " M17.29 21.02c.12-.6.43-2.3.5-3.02 M2 12a10 10 0 0 1 18-6 M2 16h.01"
                   + " M21.8 16c.2-2 .131-5.354 0-6 M5 19.5C5.5 18 6 15 6 12a6 6 0 0 1 .34-2"
                   + " M8.65 22c.21-.66.45-1.32.57-2 M9 6.8a6 6 0 0 1 9 5.2v2"
            waiting: loginGroup.fpState === 1
            failed: loginGroup.fpState === 2
            passed: loginGroup.fpState === 4
            dimmed: loginGroup.fpState === 3
        }

        BioButton {
            visible: loginGroup.faceState !== 0
            // 取景框 + 眼睛、鼻子、嘴
            glyph: "M3 8V5.5A2.5 2.5 0 0 1 5.5 3H8 M16 3h2.5A2.5 2.5 0 0 1 21 5.5V8"
                   + " M21 16v2.5a2.5 2.5 0 0 1-2.5 2.5H16 M8 21H5.5A2.5 2.5 0 0 1 3 18.5V16"
                   + " M8.5 9v1.5 M15.5 9v1.5 M12 9v4h-1 M9 16c1.6 1.4 4.4 1.4 6 0"
            waiting: loginGroup.faceState === 2
            failed: loginGroup.faceState === 3
            passed: loginGroup.faceState === 5
            dimmed: loginGroup.faceState === 4
            clickable: loginGroup.faceState === 1 || loginGroup.faceState === 3
            onClicked: BioUnlock.startFace()
        }
    }

    component BioButton: Item {
        id: btn
        property string glyph
        property bool waiting: false   // 呼吸：等待按压 / 识别中
        property bool failed: false
        property bool passed: false
        property bool dimmed: false
        property bool clickable: false
        signal clicked()

        width: 44
        height: 44
        opacity: dimmed ? 0.4 : 1
        readonly property color tint: failed ? "#ff7a7a" : passed ? "#62dc80" : "white"

        Rectangle {
            anchors.fill: parent
            radius: width / 2
            color: Qt.rgba(1, 1, 1, btn.clickable && mouse.containsMouse ? 0.3 : 0.15)
            border.width: 1
            border.color: btn.failed || btn.passed ? btn.tint : Qt.rgba(1, 1, 1, 0.3)
        }

        Item {
            id: pulse
            anchors.fill: parent
            transform: Translate { id: shift }

            Shape {
                width: 24
                height: 24
                anchors.centerIn: parent
                preferredRendererType: Shape.CurveRenderer
                ShapePath {
                    strokeColor: btn.tint
                    strokeWidth: 1.5
                    fillColor: "transparent"
                    capStyle: ShapePath.RoundCap
                    joinStyle: ShapePath.RoundJoin
                    PathSvg { path: btn.glyph }
                }
            }

            SequentialAnimation on opacity {
                running: btn.waiting
                loops: Animation.Infinite
                NumberAnimation { to: 0.35; duration: 900; easing.type: Easing.InOutSine }
                NumberAnimation { to: 1; duration: 900; easing.type: Easing.InOutSine }
            }
        }

        onWaitingChanged: if (!waiting) pulse.opacity = 1
        onFailedChanged: if (failed) shake.restart()

        SequentialAnimation {
            id: shake
            NumberAnimation { target: shift; property: "x"; to: -6; duration: 50 }
            NumberAnimation { target: shift; property: "x"; to: 6; duration: 80 }
            NumberAnimation { target: shift; property: "x"; to: -4; duration: 70 }
            NumberAnimation { target: shift; property: "x"; to: 3; duration: 60 }
            NumberAnimation { target: shift; property: "x"; to: 0; duration: 50 }
        }

        MouseArea {
            id: mouse
            anchors.fill: parent
            enabled: btn.clickable
            hoverEnabled: true
            onClicked: btn.clicked()
        }
    }

    /*****************************/
    /* Functions and Connections */
    /*****************************/

    function resetHint() {
        hintText.text = loginGroup.noPasswdLogin ? "" : normalHint
    }

    function updateUser() {
        loginGroup.enteringOtherUser = false
        loginGroup.showUserNotFoundError = false
        let currentUser = UserModel.get(UserModel.currentUserName)
        username.text = currentUser.realName.length === 0 ? currentUser.name : currentUser.realName
        passwordField.text = ''
        avatar.fallbackSource = currentUser.icon
        loginGroup.noPasswdLogin = currentUser.noPassword === true
        resetHint()
        if (loginGroup.noPasswdLogin)
            noPwdLoginBtn.forceActiveFocus()
    }

    function startOtherUserMode() {
        loginGroup.enteringOtherUser = true
        loginGroup.noPasswdLogin = false
        loginGroup.showUserNotFoundError = false
        username.text = qsTr("Enter username")
        passwordField.text = ""
        passwordField.forceActiveFocus()
    }

    function confirmOtherUser() {
        let name = passwordField.text.trim()
        if (name.length === 0) return
        if (UserModel.tryAddNssUser(name)) {
            UserModel.currentUserName = name
            // updateUser() fires via currentUserNameChanged, resets enteringOtherUser
        } else {
            loginGroup.showUserNotFoundError = true
            hintText.text = qsTr("User not found.")
            passwordField.selectAll()
            passwordField.forceActiveFocus()
        }
    }

    function userLogin() {
        let user = UserModel.get(UserModel.currentUserName)
        let pwd = loginGroup.noPasswdLogin ? "" : passwordField.text
        if (user.loggedIn)
            GreeterProxy.unlock(user.name, pwd)
        else
            GreeterProxy.login(user.name, pwd, SessionModel.currentIndex)
    }

    Connections {
        target: GreeterProxy
        function onFailedAttemptsChanged (attempts) {
            if (attempts > 0) {
                passwordField.selectAll()
                if (loginGroup.activeFocus) {
                    passwordField.forceActiveFocus()
                }
                hintText.text = qsTr("Password is incorrect.")
            } else {
                passwordField.text = ""
                resetHint()
            }
        }
    }

    Connections {
        target: UserModel

        function onUpdateTranslations(locale) {
            updateUser()
        }

        function onCurrentUserNameChanged(name) {
            updateUser()
        }
    }

    Component.onCompleted: {
        updateUser()
    }

    onActiveFocusChanged: {
        if (activeFocus) passwordField.forceActiveFocus()
    }
}
