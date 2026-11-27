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

## 指纹

汇顶 HC460，走 SPI + TEE，不经过 fprintd/libfprint。需要从 UOS 复制 `/vendor`、`/usr/lib` 下的厂商库（`libfingerprint.so`、`libhwFingerprint.so`、`libteec.so*` 等）以及 `hwfp_init.sh`、`hwfp_init.service`、`teecd.service`，补一个 `/usr/bin/fingerprint_daemon` 符号链接（指向 `/vendor/bin/hw/vendor.huawei.hardware.biometrics.fingerprint@2.1-service`），再 enable `hwfp_init.service`。本仓库没有把这一步自动化。

- 能识别后，sudo 和 polkit 可以用指纹；**锁屏和登录界面不行**：ddm + treeland 的 greeter 不接 `Authenticate1`，只能输密码。
- UOS 和 deepin 共用 TEE 里的指纹存储，在一边重新录入会影响另一边，动手前先备份。

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

## 内核和 grub

- deepin 的内核包装上也用不了（驱动都在厂商内核里），而 `/etc/kernel/postinst.d/zz-update-grub` 会在装内核时触发 `update-grub`，可能改写 UOS Boot 分区的启动项。`hisi-patch-check` 会提示这个风险，并校验 UOS Boot 分区上的 `custom.cfg`。
- 只用 `grub-reboot` 进 deepin，不要改默认启动项，也不要在 deepin 里运行 `update-grub`。
