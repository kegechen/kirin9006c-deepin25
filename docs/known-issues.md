# 已知问题和注意事项

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

## 指纹

汇顶 HC460，走 SPI + TEE，不经过 fprintd/libfprint。需要从 UOS 复制 `/vendor`、`/usr/lib` 下的厂商库（`libfingerprint.so`、`libhwFingerprint.so`、`libteec.so*` 等）以及 `hwfp_init.sh`、`hwfp_init.service`、`teecd.service`，补一个 `/usr/bin/fingerprint_daemon` 符号链接（指向 `/vendor/bin/hw/vendor.huawei.hardware.biometrics.fingerprint@2.1-service`），再 enable `hwfp_init.service`。本仓库没有把这一步自动化。

- 能识别后，sudo 和 polkit 可以用指纹；**锁屏和登录界面不行**：ddm + treeland 的 greeter 不接 `Authenticate1`，只能输密码。
- UOS 和 deepin 共用 TEE 里的指纹存储，在一边重新录入会影响另一边，动手前先备份。

## 网络

- deepin 自带的 wpa_supplicant 2.10 会建 `p2p-dev-wlan0`，Hi1103 驱动处理不了，卡死后占住 rtnl，所有网络相关进程跟着卡。所以换成 UOS 带 `+hisi` 补丁的 2.9。
- Hi1103 对 `NL80211_CMD_GET_STATION + NLM_F_DUMP` 不会结束（`iw station dump` 3 秒刷出 57 万条），NM 1.44 主线程会 100% CPU，`nmcli` 显示“未运行”，控制中心卡死。`libnm-station-fix.so` 把它改成带 BSSID 的单次查询。
- `nmcli device reapply wlan0` 会因为 `802-11-wireless.powersave` 属性报错，改 DNS 后用 `resolvectl dns wlan0 ...` 临时生效即可。
- 热点“能 ping 通 IP、域名解析失败”通常是 Windows ICS 的 DNS 代理不响应：只开关热点没用，要管理员 `Restart-Service SharedAccess -Force` 再开热点；或者在 ARM 端用固定 DNS（见 `examples/`）。

## 内核和 grub

- deepin 的内核包装上也用不了（驱动都在厂商内核里），而 `/etc/kernel/postinst.d/zz-update-grub` 会在装内核时触发 `update-grub`，可能改写 UOS Boot 分区的启动项。`hisi-patch-check` 会提示这个风险，并校验 UOS Boot 分区上的 `custom.cfg`。
- 只用 `grub-reboot` 进 deepin，不要改默认启动项，也不要在 deepin 里运行 `update-grub`。
