// 控制中心“账户 → 登录方式 → 手机解锁”入口（插件格式 v1.0：plugins_v1.0/phoneunlock/）。
// 这个文件里不能用 dccData，只放入口本身；页面内容在 phoneunlockMain.qml。
import org.deepin.dcc 1.0

DccObject {
    name: "phoneunlock"                     // 和插件目录名一样
    parentName: "accountsloginMethod"       // 当前账户的“登录方式”组（accounts 插件的 LoginMethod.qml）
    displayName: qsTr("手机解锁")
    description: qsTr("用手机上的指纹解锁这台电脑")
    weight: 40                              // 排在“生物认证”（30）后面
}
