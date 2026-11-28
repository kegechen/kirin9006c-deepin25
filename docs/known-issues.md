# 已知问题和注意事项

## ddm 0.4：开机偶发不出登录界面（段错误）

**现象**：开机后屏幕停在控制台或 grub 留下的画面，`systemctl --failed` 里有 `ddm.service`，日志是 `dde-seatd control event socket error: QLocalSocket::ServerNotFoundError "...Invalid name"`，紧接着 `code=killed, signal=SEGV`。

**原因**：`dde-seatd.service` 是 `Type=simple`，ddm 单元里的 `After=dde-seatd.service` 只保证 dde-seatd 进程已经拉起，不保证 `/run/dde-seatd-control.sock` 已经建好。ddm 连不上控制 socket 会直接崩溃，而原单元的 `StartLimitBurst=2`（30 秒）让它开机时连撞两次就被放弃。平时第二次重启多半能连上，开机变慢一点（比如开了 systemd debug 日志）就会两次都撞上。

**处理**：`/etc/systemd/system/ddm.service.d/hisi-seatd-wait.conf` 在启动前最多等 10 秒控制 socket 出现，并把重启间隔改成 1 秒、次数限制放宽到 60 秒 10 次。当场恢复：`sudo systemctl reset-failed ddm; sudo systemctl start ddm`。

## ddm 0.4：注销后重新登录，锁屏只剩一张不动的壁纸

**现象**：开机后第二次走 Login（手动注销再登录、ddm 重启后登录）时，登录界面没有输入框，只剩一张静止的壁纸；或者报 `Failed to get auth info from display manager`。普通的锁屏解锁（Unlock）不受影响。

**原因**（ddm 0.4.0 `Auth::openSession`）：ddm 为新会话 fork 出会话领头进程（root），领头进程走 PAM 打开会话，再 fork 出用户身份的子进程去 exec `dde-session`（或 PAM 模块拉起的 `gnome-keyring-daemon`）。出问题时这个子进程**还没 exec 就卡在 futex 上**（exe 仍是 `/usr/bin/ddm`），领头进程停在 `wait4`，而 ddm 主线程在同步读管道等会话 ID（内核栈停在 `pipe_read`），整个 daemon 不再处理 greeter 消息。fork 之后具体卡在哪把锁还没抓到。

**处理**：`hisi-ddm-watchdog.service` 每 5 秒扫一次，满足“非 root、exe 仍是 `/usr/bin/ddm`、父进程也是 `/usr/bin/ddm`、存活超过 15 秒”的进程，先把内核栈和 gdb 用户态栈写进 `/var/log/hisi-ddm-watchdog.log`，再 SIGKILL。正常情况下这类子进程几毫秒内就会 exec，不会误判。结束它之后领头进程会继续拉起 dde-session，堆积的 greeter 消息也会被处理掉。

- 手动执行一次：`sudo STUCK_SECS=5 hisi-ddm-watchdog --once`
- 判断 ddm 是否卡住：`sudo cat /proc/$(pgrep -xo ddm)/stack`，正常应停在 `ppoll`/`do_sys_poll`，卡住时是 `pipe_read`。
- 再次出现时请先看 `/var/log/hisi-ddm-watchdog.log` 里的 gdb 栈，那是定位根因的线索。

## 动态壁纸（treeland 0.11）

- **桌面上不动是设计**：锁屏界面显示时循环播放；回到桌面只在“显示桌面”那一刻播放，随即 `slowDown()`，约 0.3 秒后定格。换工作区、换壁纸时只要没锁屏也会直接暂停。
- **不要手动杀 `treeland-wallpaper-factory`**：treeland 会重新拉起它，但 `WallpaperItem::updateSurface()` 只按路径比较，不会重新挂 surface。新进程在没人显示的情况下持续解码，桌面和锁屏还挂着已销毁的旧 surface。恢复：控制中心先换一张静态壁纸再切回，或者 `sudo systemctl restart treeland`。
- **`wallpaper failed ... source already configured`** 是协议里的 `already_used` 通知，桌面和锁屏各请求一次，第二次必报，不是故障。
- **切换动态壁纸后桌面变黑**：遇到过一次，旧壁纸进程跑了两小时、还在用 llvmpipe。重启 treeland 后恢复，新进程走 libmali。根因没确认，怀疑是 surface ready 后桌面项立刻 `setPlay(false)`，mpv 还没渲染出第一帧。再遇到时先试“换静态壁纸再切回”。

## 调试时不要以 root 运行 treeland 二进制

会话运行期间，哪怕只是 `sudo treeland --version`，也会惊动 ddm，正在运行的 treeland 丢掉 DRM master，任务栏 `dde-shell@DDE` 随之崩溃并触发 start-limit。恢复：

```sh
systemctl --user reset-failed dde-shell@DDE.service
systemctl --user start dde-shell@DDE.service
```

`sudo systemctl restart treeland` 是安全的：会回到锁屏，大部分会话进程会保留。重启后建议确认 ddm 没有卡在 `pipe_read`（见上文）。

## 指纹 / 人脸

指纹是汇顶 HC460，走 SPI + TEE，不经过 fprintd/libfprint。需要从 UOS 复制 `/vendor`、`/usr/lib` 下的厂商库（`libfingerprint.so`、`libhwFingerprint.so`、`libteec.so*` 等）以及 `hwfp_init.sh`、`hwfp_init.service`、`teecd.service`，补一个 `/usr/bin/fingerprint_daemon` 符号链接（指向 `/vendor/bin/hw/vendor.huawei.hardware.biometrics.fingerprint@2.1-service`），再 enable `hwfp_init.service`（一键装机脚本会照 UOS 的启用状态做）。人脸用 deepin 自带的 `deepin-face`（按需由 D-Bus 拉起，用普通摄像头）。

- **录入入口**：控制中心“账户 → 登录方式 → 生物认证”。dde-control-center 在 treeland 会话里把它写死藏掉了（`!DccApp.isTreeland()`），`hisi-dcc-bio-entry` 在插件上改两个字节放出来；dde-control-center 升级会覆盖，`hisi-patch-check` 报“改动”，`--fix` 重打。
- **锁屏**：treeland 0.11 锁屏只有密码框，ddm 拿到密码一次性走 PAM，`Authenticate1` 的生物识别轮不到。`hisi-bio-unlock.service` 在后台补上：监听 logind 的 Lock，用 `Authenticate1` 建认证 session，指纹一直等待；人脸只在锁屏 2 秒后有键盘 / 触控板动作、开盖或唤醒时开，每次最多 15 秒（避免手动锁屏后人还在跟前就被立刻解开）；认证通过后调 logind `UnlockSession`，treeland 随即收起锁屏。用密码解锁时释放传感器和摄像头，并清零失败次数。
  - **锁屏界面**：`libtl-lockbio.so`（预加载进 treeland，不改 treeland 的包文件）在密码框下面加了指纹 / 人脸图标和提示，类似手机：指纹图标呼吸表示在等，按上去就验证；人脸在识别时提示“正在识别人脸”，没开或没通过时点人脸图标再识别一次（最多 15 秒）；不匹配图标变红并晃一下，通过变绿后锁屏收起；连错被锁时图标变灰，提示改用密码。密码框照常可用。只显示该用户录入过的类型，都没录入就和原来一样只有密码框。
    - 做法：Qt 资源按登记先后查找，它在 treeland 加载锁屏插件之前登记一份 `LockScreen` 模块清单（`src/lockbio/qmldir`），把 `UserInput` 换成 `src/lockbio/UserInputHisi.qml`（照 treeland 0.11.0 的 `UserInput.qml` 改），其余组件照旧用插件自带的。状态来自 `hisi-bio-unlock` 在 system bus 上的 `org.hisi.BioUnlock1`（`GetState` / `StateChanged` / `StartFace`，策略只放行 root 和 greeter 用户 dde）；认证和解锁仍在 `hisi-bio-unlock` 里做，界面只负责显示和“点人脸”。
    - 自我保护：锁屏插件 `liblockscreen.so` 的 sha256 不在 `/usr/local/lib/hisi-treeland/lockbio.known` 里（treeland 升级了）、缺 `qml6-module-qtquick-shapes`、或上一次 treeland 启动没撑过 60 秒（标记文件 `/var/lib/ddm/.hisi-lockbio-starting`）时都不启用，锁屏退回只有密码框，后台指纹 / 人脸解锁照常；`hisi-patch-check` 会给出警告。treeland 升级后：对照新版 `UserInput.qml` 复核 `UserInputHisi.qml` 和 `qmldir`，再把新 sha256 加进 `lockbio.known`。临时关掉：treeland 的 drop-in 里加 `Environment=HISI_LOCKBIO_DISABLE=1`。
    - 已用测试构建（`-DHISI_BIO_SELFTEST` 多一个 `DebugSetState` 方法）在真机上逐个截图核对过各状态的图标和提示，点人脸图标能调到 `StartFace`，密码解锁不受影响。
    - `systemctl restart treeland`（或 treeland 崩溃重启）之后直接出现的锁屏不经过 logind 的 Lock，`hisi-bio-unlock` 不知道已经锁屏，这一次只能输密码；正常锁屏（快捷键、合盖、空闲）都会走 logind，不受影响。
  - 只用该用户在 deepin 里录入过的类型（服务端按录入情况决定，读 session 的 `FactorsInfo`）；没录入时什么都不做，录入后下一次锁屏就生效，不用重启服务。日志：`journalctl -u hisi-bio-unlock`。
  - 指纹 / 人脸各自连错 3 次被锁，要用一次密码。
  - “认证通过 → 解锁”这条路已用密码因子自测过（`-DHISI_BIO_SELFTEST` 编译）；真实指纹 / 人脸解锁要录入之后再实测。
- **登录界面**（开机 / 注销后）仍只能输密码。
- sudo 和 polkit 走 `pam_deepin_authentication`，可以用指纹。
- UOS 和 deepin 共用 TEE 里的指纹存储，在一边重新录入会影响另一边，动手前先备份。

## 手机指纹解锁

设计和协议见 [phone-unlock.md](phone-unlock.md)。配对：电脑上 `sudo hisi-phone pair`，手机 App 扫码（没装 qrencode 时把打印的链接发到手机，在 App 里粘贴）。

- **Wi‑Fi 要和电脑在同一网段**，路由器 / 热点不能开 AP 隔离；不在同一网段就走蓝牙。手机 App 也能手填额外地址（调试时配合 `adb reverse` 用 `127.0.0.1`）。
- 蓝牙：电脑一直以 BLE 外设广播（BlueZ 5.85，`hisi-phone-link` 通过 D-Bus 注册 GATT 服务和广播，不需要手机和电脑先“配对蓝牙”）。蓝牙关着时只剩 Wi‑Fi，打开后自动重新注册。
  - 5.4 内核的扩展广播有毛病：每次只播一段时间（默认 2 秒，这里设 60 秒）且到期不重开，“停止全部广播”控制器又不认。`hisi-phone-link` 掐着到期时刻注销再注册续上；刚重启服务时上一个进程的广播还在播，前一分钟内 bluetoothd 日志里会有一串 `Failed to add advertisement: Busy (0x0a)`，是正常的等待。
  - 同样因为内核，空中的广播不带服务 UUID、间隔固定 1.28 秒：手机只能按二维码里的蓝牙地址直连（不影响使用），连上要 1～6 秒，比 Wi‑Fi 慢。
- 只在该用户有本地图形会话时能解锁；锁屏截图只在 `hisi-bio-unlock` 确认已锁屏时给（和指纹 / 人脸一样，`systemctl restart treeland` 之后那次锁屏它不知道，App 上看不到截图，但仍可尝试解锁）。
- 手机上新录入或删除指纹后，Keystore 里的密钥会失效，要重新配对；`sudo hisi-phone list` / `unpair <ID>` 管理已配对的手机。
- 已用 `tools/phone-unlock-test.py`（软件密钥扮演 App）在真机上走通 Wi‑Fi：配对 → HELLO → 锁屏截图 → 错签名被拒 → 正确签名解锁。日志：`journalctl -u hisi-bio-unlock -u hisi-phone-link`。
- 不用手机解锁：`sudo systemctl disable --now hisi-phone-link`（停掉 47390 端口和蓝牙广播）。
- App 的解锁界面有 **一键锁屏**（不验指纹，方便测试）；长按电脑卡片还能开 **离开自动锁屏**：手机 App 用蓝牙保持连接，手机走远、蓝牙断开后电脑等 15 秒～5 分钟自动锁屏（Windows 动态锁那种），期间连回来就不锁；只会锁不会解锁。关掉开关时会先通知电脑再断开，不会误锁。
- 离开自动锁屏开着时不要随手用 `adb install -r` 更新 App：华为的启动管理默认拦侧载 App 自启动，更新会把服务杀掉且收不到 `MY_PACKAGE_REPLACED`，电脑就按“手机离开”锁屏了。要先关掉开关再装；华为 / 荣耀要在 设置 → 应用 → 应用启动管理 里把 App 改成手动管理、全部允许。

## 声音

内核驱动和固件都是 UOS 的，本来就正常；没声音和杂音都出在用户态。一键装机已处理下面三件事，手工装的照着补。

1. **没声音**：UOS 的 `hw-audio` 包在 postinst 里给 `hwaudioservice` 设了 `cap_dac_override`，只拷文件会丢掉。没有这个能力，它读不了 DMI 表判断机型，两颗 TFA9874 功放（I2C 总线 3 的 0x34/0x36，`STATUS` 寄存器 0x0001 = 没上电）就一直不工作。补上之后，登录时由 `/etc/xdg/autostart/hwaudioservice.desktop` 自动拉起，不需要 root、也不用放宽 DMI 表权限：
   ```sh
   sudo setcap cap_dac_override=eip /usr/bin/hwaudioservice
   sudo setcap cap_dac_override=eip /usr/share/hw-audio/hwaudioservice
   ```
   日志（`/tmp/hwaudioservice-*.log`）里的 `smartpakit ... not support cmd = 0x835c4d01` 在 UOS 上也有，不影响功放上电。
2. **PipeWire 用不上华为配置集**：UOS 的 `90-pulseaudio-huawei.rules` 只设 PulseAudio 认的 `PULSE_PROFILE_SET`，PipeWire 只认 `ACP_PROFILE_SET`，于是退回 `stereo-fallback`。`91-pipewire-huawei.rules` 补上这个变量；配置集和 path 要放进 ACP 的目录：
   ```sh
   A=/usr/share/alsa-card-profile/mixer
   sudo install -m 644 /media/uos/Roota/usr/share/pulseaudio/alsa-mixer/profile-sets/pulse-huawei.conf $A/profile-sets/
   sudo install -m 644 /usr/share/pulseaudio/alsa-mixer/paths/*-huawei*.conf $A/paths/
   ```
   UOS 上的 `pulse-huawei.conf` 是 postinst 按机型建的符号链接（L540 指向 `pulse-huawei-6-channels.conf`）。
3. **杂音、播放变快**：`da_combine_v5` 驱动的 DMA 周期写死为 960 帧（20ms），却按 ALSA 协商的周期推进 `hw_ptr`。用 `aplay -D hw:0,0 --period-size=N` 放一个 10 秒的文件：960 帧用时 10.01 秒，1024 帧 9.39 秒，1920 帧 5.01 秒。PipeWire 默认用 1024，于是断流、噼啪、快 6.7%。`90-hisi-da-combine.conf` 把周期固定为 960 帧、2 个周期，并改为按中断调度（`api.alsa.disable-tsched`）。
   - 只看 `pw-top` 的 ERR 列会误判：1920 帧时 ERR 也是 0，但实际是双倍速。要看 `/proc/asound/card0/pcm0p/sub0/status` 里 `hw_ptr` 每秒涨多少（应约 48000），或者量一个文件实际放了多久。

手工改完不用重启：`sudo udevadm control --reload; sudo udevadm trigger --action=change --subsystem-match=sound`，再 `systemctl --user restart wireplumber`，默认输出选 DA_combine 的模拟立体声。

## 功能键

- **Fn 是切换键，不是按住用的**：按一下 Fn 在“F1–F12”和“热键”两种模式间切换（键盘发 HID 用法 0x700ae / 0x700af，内核映射成 `KEY_UNKNOWN`）。热键模式下 F4/F5/F6 直接发 `KEY_MUTE` / `KEY_VOLUMEDOWN` / `KEY_VOLUMEUP`，音量调节正常；按住 Fn 再按 F4 得到的是普通 F4。
- **亮度键无效**：键能收到，但 treeland 下 `dde-shortcut-tool` 只通过合成器的 gamma 调亮度，而 hisi-drm 的 eDP-1 不支持 gamma LUT（treeland 日志 `does not support gamma LUT`），请求被拒，背光 `/sys/class/backlight/mipi2edp-backlight` 没人去写。暂不处理。

## 网络

- deepin 自带的 wpa_supplicant 2.10 会建 `p2p-dev-wlan0`，Hi1103 驱动处理不了，卡死后占住 rtnl，所有网络相关进程跟着卡。所以换成 UOS 带 `+hisi` 补丁的 2.9。
- Hi1103 对 `NL80211_CMD_GET_STATION + NLM_F_DUMP` 不会结束（`iw station dump` 3 秒刷出 57 万条），NM 1.44 主线程会 100% CPU，`nmcli` 显示“未运行”，控制中心卡死。`libnm-station-fix.so` 把它改成带 BSSID 的单次查询。
- `nmcli device reapply wlan0` 会因为 `802-11-wireless.powersave` 属性报错，改 DNS 后用 `resolvectl dns wlan0 ...` 临时生效即可。
- 热点“能 ping 通 IP、域名解析失败”通常是 Windows ICS 的 DNS 代理不响应：只开关热点没用，要管理员 `Restart-Service SharedAccess -Force` 再开热点；或者在 ARM 端用固定 DNS（见 `examples/`）。

## 玲珑应用（记事本、相机、计算器等）一打开就退出

deepin 25 的记事本、相机、计算器、日历、看图、影院等都是玲珑包，同时坏掉；deb 装的软件（比如微信）不受影响。`ll-cli run org.deepin.editor` 只报 `Error -1: InitRunContext failed`，journal 里是：

```
ll-package-manager: failed to remove /tmp/linglong-runtime-991/linglong/<容器ID>.init/rootfs/etc/localtime: 不允许的操作
ll-package-manager: init run context failed: ... failed to create symlink .../rootfs/etc/localtime -> /usr/share/zoneinfo/...: 文件已存在
```

原因是一键装机在 UOS 下运行，而 UOS 开着 SELinux，解包和 chroot 里写到新分区的文件都被打上 `security.selinux="root:object_r:unlabeled_t:s0"`（`ls -l` 权限位后面多一个 `.`）。deepin 不加载 SELinux（LSM 只有 `capability,elfverify,apparmor,uosmanager`），这时内核 `cap_inode_setxattr` 要求有 `CAP_SYS_ADMIN` 才能写 `security.*` 属性。5.4 内核不能在用户命名空间里挂内核 overlay（linglong 要求 ≥ 5.11），于是退回 fuse-overlayfs；`ll-package-manager` 以普通服务账号删除 base 层的 `etc/localtime` 前要把 `etc` 目录 copy-up，连同这个属性一起复制，失败就是 EPERM。

处理：`hisi-strip-selinux-xattr` 只在根分区内清除 `security.selinux`，不跨挂载点（`/home` 在 `/data` 上、和 UOS 共用，不会碰到）。一键装机会 enable `hisi-strip-selinux-xattr.service`，deepin 首次开机时跑一次。之前装的系统、手工装的系统执行一次：

```sh
sudo hisi-strip-selinux-xattr /          # 约 28 万个文件，十几秒；加 -n 只统计
```

不用重启，直接重新打开应用即可。这一步不能放在 UOS 下做：SELinux 加载着时，内核不允许删除 `security.selinux`，permissive 也不行。玲珑仓库是 `bare-user-only` 模式，xattr 不参与校验和，清掉不影响 `ll-cli` 的校验和升级。

## 内核和 grub

- deepin 的内核包装上也用不了（驱动都在厂商内核里），而 `/etc/kernel/postinst.d/zz-update-grub` 会在装内核时触发 `update-grub`，可能改写 UOS Boot 分区的启动项。`hisi-patch-check` 会提示这个风险，并校验 UOS Boot 分区上的 `custom.cfg`。
- 只用 `grub-reboot` 进 deepin，不要改默认启动项，也不要在 deepin 里运行 `update-grub`。
