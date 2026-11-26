# kirin9006c-deepin25

让 **deepin 25** 跑在 **华为擎云 L540（麒麟 9006C / ARM64）** 上的补丁集：垫片源码、systemd drop-in、守护脚本和补丁自检工具。

这台机器出厂预装 UOS，UFS、显示、GPU、Wi‑Fi 驱动全部编进厂商内核 `5.4.96-arm64-desktop`，deepin 25 的通用 ISO 连硬盘都看不到。这里的思路是**换用户态、不换内核**：把 deepin 25 的根文件系统装进 Rootb 分区，沿用 UOS 的内核、initrd、固件和 `/lib/modules`，再逐个修掉用户态和厂商驱动之间的不兼容。

排障过程和每个坑的来龙去脉见博客：[在 ARM 笔记本上给 deepin 25 换心不换骨：麒麟 9006C 驱动兼容踩坑全记录](https://kegechen.github.io/2026/11/25/kylin-9006c-deepin25-arm-pitfalls/)。

> ⚠️ 这是一台机器上逐步调出来的结果，不是安装器。全新机器从头走一遍的完整流程没有重新验证过。动手前先备份 Rootb 和 `/data`，并保持 grub 默认启动 UOS。

## 适用环境

| 项目 | 版本 |
|---|---|
| 机器 | 华为擎云 L540-001，麒麟 9006C（`hisilicon,baltimore`），Mali-G78 22 核，Hi1103 Wi‑Fi/蓝牙 |
| 内核 | UOS 厂商内核 `5.4.96-arm64-desktop`（含 elfverify LSM、`RT_GROUP_SCHED`） |
| 用户态 | deepin 25.3 arm64，装在 Rootb |
| 会话 | ddm 0.4.0 + treeland 0.11.0（捆绑 wlroots 0.20.2，即 `libwaylib-wlroots.so.0.20.2`） |
| 网络 | NetworkManager 1.44，UOS 的 wpa_supplicant 2.9（+hisi） |

`tl-malibuf` 按 wlroots 0.20.2 的结构体布局编写，treeland/wlroots 升级后需要复核（`hisi-patch-check` 会检查版本和符号）。其他机型可以参考思路，文件不要直接照搬。

## 现状

| 功能 | 状态 | 相关文件 |
|---|---|---|
| 启动 | 正常；grub 默认仍是 UOS，deepin 每次正常开机后自动把下次开机设回 deepin，开机卡死则自动回 UOS | `d25-init`、`hisi-next-boot`、`examples/grub-custom.cfg` |
| Wi‑Fi | 正常 | UOS 的 `wpa_supplicant-hisi`、`libnm-station-fix.so` |
| 蓝牙 | 正常 | UOS 的 `hciattach-hisi` |
| 合成器 GPU | treeland 走 Mali-G78，CPU 从约 500%（llvmpipe）降到 3–6% | `libmali-compat.so`、`libtl-norender.so`、treeland drop-in |
| 客户端 GPU | 任务栏、控制中心、壁纸进程、mpv 等 Wayland 客户端不设环境变量即走 libmali | `libtl-malibuf.so`、`libEGL_hisi_mali.so.0` |
| 动态壁纸 | 锁屏循环播放，桌面定格（treeland 0.11 的设计）；重编码为 1440p 后解码开销降一半以上 | `hisi-wallpaper-reencode` |
| 开机登录界面 | ddm 0.4 抢在 dde-seatd 建好控制 socket 前启动会段错误，drop-in 先等 socket 再启动 | `ddm.service.d/hisi-seatd-wait.conf` |
| 注销后重新登录 | ddm 0.4 偶发卡死，由看门狗自动恢复 | `hisi-ddm-watchdog` |
| 补丁被升级覆盖 | apt 钩子 + 每日定时检查，异常时桌面通知，可一键恢复 | `hisi-patch-check` |
| 指纹 | sudo/polkit 可用；锁屏和登录界面只能输密码 | 见 [docs/known-issues.md](docs/known-issues.md) |
| Xwayland | 仍是软件渲染 | — |
| 视频解码 | 纯软件（deepin 的 ffmpeg 6.1 没有 OMX 解码器） | — |

实测 CPU 占用（2160×1440 屏幕）：

| 场景 | 之前 | 现在 |
|---|---|---|
| treeland 合成器 | ~500%（llvmpipe） | 3–6% |
| 任务栏 dde-shell | Mesa llvmpipe | 0–8% |
| 控制中心壁纸页 | 66%（Mesa） | 30% |
| mpv 全屏播放 4K 壁纸 | 500%（llvmpipe） | 111–120%（libmali，软解） |
| 同上，壁纸重编码为 1440p | — | ~42% |

## 目录结构

```text
install-deepin25.sh       一键装机（在 UOS 下运行，见下文“一键装机”）
src/                      垫片源码（在目标机上编译）
  hisi-mali-egl.c           glvnd EGL vendor：让 Wayland 客户端用厂商 libmali
  mali-compat.c             补齐 libmali 与 deepin 用户态的接口差（treeland 预加载）
  tl-malibuf.c              给 treeland 补上 libmali 客户端需要的私有协议 mali_buffer_sharing v5
  tl-norender.c             让 wlroots 不用假 render 节点、dmabuf 导出补 O_RDWR
  nm-station-fix.c          NetworkManager：把永不结束的 nl80211 station dump 改成单次查询
rootfs/                   按原路径安装的文件（/etc、/usr/local）
  etc/systemd/system/...    treeland / NetworkManager / wpa_supplicant / hciattach / ddm 的 drop-in，
                            hisi-ddm-watchdog、hisi-patch-check 的单元
  etc/glvnd/egl_vendor.d/   10_hisi_mali.json（让 glvnd 先问 libmali 垫片，再问 Mesa）
  etc/hisi-mali/egl-deny    GPU 黑名单（用户可编辑）
  etc/NetworkManager/       关闭扫描随机 MAC、关闭 Wi‑Fi 省电
  etc/apt/apt.conf.d/       hisi-patch-check 的 apt 钩子
  usr/local/sbin/           d25-init、hisi-ddm-watchdog、hisi-wallpaper-reencode、hisi-patch-check
  usr/local/lib/hisi-patch/ 补丁自检的跟踪清单、敏感包清单和说明
scripts/fetch-uos-blobs.sh 从 UOS 分区复制厂商闭源组件（本仓库不含任何厂商二进制）
examples/                 grub 入口、兜底 DNS 的示例（含占位符，按机器修改）
trial-boot/               最初试启动用的探针和“10 分钟没网就重启回 UOS”的兜底服务
tools/                    验证和测速脚本（GL 路径、CPU 占用、解码开销、gdb 抓崩溃）
legacy/                   已被 glvnd 垫片取代的 LD_PRELOAD 客户端方案，留作参考
docs/                     GPU 方案细节、已知问题
```

## 一键装机

同款机器（擎云 L540、出厂 UOS、内核 `5.4.96-arm64-desktop`）可以用 [install-deepin25.sh](install-deepin25.sh) 从零装好。ISO 自己准备（deepin 25 arm64 桌面版），UOS 需要已开启开发者模式（要用 sudo）。在 UOS 下以普通用户运行：

```sh
bash install-deepin25.sh --iso ~/deepin-25-arm64.iso                      # 装到出厂的 Rootb 分区
bash install-deepin25.sh --iso ~/deepin-25-arm64.iso --target /dev/sdb1   # 或装到任意一个分区（U 盘、内置盘空闲分区）
```

- 一共 12 步，每步打印正在做什么和总进度；前两步只做检查，第 3 步列出要清空的分区（型号、容量、所在磁盘），输入大写 `YES` 才继续。
- 目标分区会被 `mkfs.ext4` 清空；UOS 的根、Boot、EFI、/data、swap 和任何已挂载的分区都会被拒绝。
- 垫片全部在新系统里现场编译，厂商 blob 从本机 UOS 复制，仓库不带任何二进制。sudo 密码只在终端输入一次，不落盘。
- 启动项最后一步才写进 UOS Boot 分区的 `/grub/custom.cfg`（不跑 `update-grub`、不改默认项），之前任何一步失败 UOS 都没有改动。
- 结束时打印回退方式；也存在 `/var/lib/deepin25-onekey/run-*/INSTALL_RESULT.txt`。
- 装在 USB 设备上属于实验性（UOS initrd 带 usb-storage 时才允许），开机必须插着，deepin 不会设成“重启后继续进”。
- 默认只装 deepin 25 和本仓库的补丁。天气壁纸是可选项、默认不装（也不会自动探测）：明确加 `--weather-wallpaper <WeatherWallpaper 源码目录>` 才会多一步现场编译它（多装几百 MB 的 Qt6/WebEngine，失败不影响系统）。

## 安装（手工）

前提：deepin 25 的根文件系统已经放进 Rootb，并且已从 UOS 复制了内核模块（`/lib/modules/5.4.96-arm64-desktop`）和固件。这部分可以用上一节的一键脚本完成，下面是它背后的手工步骤。

### 1. 引导（在 UOS 下操作）

1. 把 `rootfs/usr/local/sbin/d25-init` 复制到 Rootb 的 `/usr/local/sbin/`（`chmod 755`）。它在 `exec systemd` 之前用 `printf 0` 关掉 elfverify，否则 systemd 第一次 spawn 子进程就会永久卡住。
2. **让 `/sbin/init` 指向 d25-init**（在 Rootb 的 chroot 里）：
   ```sh
   dpkg-divert --local --rename --divert /usr/sbin/init.systemd --add /usr/sbin/init
   ln -s ../local/sbin/d25-init /usr/sbin/init     # 必须是相对链接
   ```
   原因：UOS initrd 的 `uos_config_init` 只有在新根的 `/boot/grub/grub.cfg` 含 `password_pbkdf2` 行时才认 `init=`，否则强制 `/sbin/init`。Rootb 一开始没有这个文件，`init=` 碰巧有效；deepin 控制中心会拉起 deepin-grub2，在 Rootb 生成一份 grub.cfg，从下次开机起 `init=` 就被忽略、elfverify 不关，systemd 卡在 “Listening on systemd-udevd-kernel.socket” 之后。
3. 参照 [examples/grub-custom.cfg](examples/grub-custom.cfg) 在 UOS Boot 分区的 `/grub/custom.cfg` 里加一个启动项（`init=/usr/local/sbin/d25-init` 可以保留，有了第 2 步之后它不再是必需的）。
4. `sudo grub-reboot deepin25-rootb` 后重启。默认启动项保持 UOS，断电或再重启就回 UOS。
   装好 `hisi-next-boot.timer`（`systemctl enable hisi-next-boot.timer`）后，deepin 每次开机 2 分钟、ddm 仍在运行时，会把 UOS Boot 分区 grubenv 的 `next_entry` 设回 `deepin25-rootb`：平时重启一直进 deepin，某次开机卡死、长按重启就回 UOS。在 deepin 里 `sudo hisi-next-boot uos` 让下次进 UOS；`/etc/hisi-next-boot.off` 存在时不自动设置。
5. 最初几次试启动可以装上 [trial-boot/](trial-boot/) 里的兜底服务。

deepin 里的 `update-grub`（包括控制中心的启动菜单设置）只会写 Rootb 自己的 `/boot/grub/grub.cfg`，开机并不读它（真正生效的是 UOS Boot 分区那份），但它会触发上面第 2 步说的 initrd 行为。Rootb 的 fstab 不要挂载 UOS Boot 分区到 `/boot`，否则 `update-grub` 会改写 UOS 的 `grub.cfg`。

### 2. 编译和安装（在 deepin 25 下操作）

```sh
sudo apt install gcc make pkg-config libwayland-dev libdrm-dev libegl-dev libglvnd-core-dev
# UOS 根分区（Roota）挂载到 /media/uos/Roota 之后：
sudo scripts/fetch-uos-blobs.sh /media/uos/Roota   # libmali、wpa_supplicant 2.9、OpenSSL 1.1、hciattach
make
sudo make install                                  # 演练：make install DESTDIR=/tmp/stage
```

`fetch-uos-blobs.sh` 和 `make install` 都先写临时文件再 rename，正在运行的 treeland、NetworkManager 不受影响。

### 3. 会话和服务

```sh
# 会话改为 ddm + treeland（lightdm + kwin_x11 下 dde 的 display 模块会因 hisi-drm 属性除零崩溃）
echo /usr/bin/ddm | sudo tee /etc/X11/default-display-manager
sudo systemctl disable lightdm
sudo systemctl enable ddm

sudo systemctl daemon-reload
sudo systemctl enable --now hisi-ddm-watchdog.service hisi-patch-check.timer
sudo systemctl enable hisi-patch-check.service
sudo systemctl restart wpa_supplicant NetworkManager
sudo systemctl restart treeland          # 会回到锁屏，大部分会话进程会保留
```

用户应用要重新登录才会改走 libmali。检查是否生效：

```sh
grep -c libmali /proc/$(pgrep -x treeland | head -1)/maps                # 合成器
grep -c libmali /proc/$(pgrep -f '[d]de-shell' | head -1)/maps           # 任务栏
```

### 4. 可选

- **动态壁纸重编码**：`sudo hisi-wallpaper-reencode`。用 `dpkg-divert` 把包里的 4K 原片挪到 `/usr/local/share/hisi-wallpapers/orig/`，再编码成 1440p（x264 `-tune fastdecode`，CRF 20），包升级不会覆盖。`--check` 只读检查，`--undo` 恢复原片。
- **兜底 DNS**：Windows 移动热点的 DNS 代理经常不响应，参照 [examples/l540-fallback-dns.conf](examples/l540-fallback-dns.conf)。

### 5. 建立补丁基线

```sh
# 可选：让它同时校验 UOS Boot 分区上的 grub 入口（把 <BOOT_UUID> 换成实际值）
sudo install -m 644 examples/boot-custom.meta /usr/local/lib/hisi-patch/meta/boot-custom.meta
sudo hisi-patch-check snapshot     # 抓取黄金副本、哈希、包版本基线
sudo hisi-patch-check              # 全量检查，0=正常 1=补丁被改动/丢失 2=仅警告
```

**以后有意修改任何补丁文件，都要再运行一次 `snapshot`**，否则 `--fix` 会把新版本还原成旧版本。

## 补丁自检：hisi-patch-check

apt 升级可能覆盖补丁（例如 treeland 升级换掉 drop-in 依赖的库，或 network-manager 升级后 drop-in 失效）。`hisi-patch-check` 会：

- 把 `files.list` 里的文件与黄金副本逐个比对（sha256、权限、符号链接），检查 fstab 受保护行、UOS Boot 分区上的 grub 入口（debugfs 只读）；
- 检查服务的 enable 状态、treeland/NetworkManager/wpa_supplicant 实际加载的环境、elfverify 状态、`libwaylib-wlroots` 版本和 9 个关键符号、敏感包版本、运行中的 treeland 和任务栏是否真的加载了 libmali、动态壁纸重编码是否完好；
- 通过 `DPkg::Post-Invoke` 钩子在每次 dpkg 事务后静默运行，装敏感包前在终端醒目提示，开机后和每天各跑一次；发现问题给桌面用户（默认 uid 1000，`HISI_DESKTOP_UID` 可改）发通知；
- `--fix` 先把现场备份到 `/var/backups/hisi-patch/<时间戳>/` 再从黄金副本恢复，不会自动重启图形和网络服务，只列出需要重启的服务。

详细说明见 [rootfs/usr/local/lib/hisi-patch/README](rootfs/usr/local/lib/hisi-patch/README)。`files.list` 和 fstab 规则是按这台机器写的（比如 box64、`/data` bind 挂载），不存在的条目会在 snapshot 时跳过。

## 开关与回退

| 想做的事 | 方法 |
|---|---|
| 某个程序不用 libmali | `HISI_MALI_DISABLE=1 程序名`，或把程序名写进 `/etc/hisi-mali/egl-deny` |
| 所有客户端都不用 libmali | `sudo touch /etc/hisi-mali/egl-off` |
| 看垫片为什么认领/放弃 | `HISI_MALI_DEBUG=1 程序名` |
| 合成器不提供 mali_buffer_sharing | 删 `treeland.service.d/l541-malibuf.conf`，daemon-reload 后重启 treeland |
| 合成器退回 llvmpipe | 见 `l540-no-rt.conf` 文件头注释 |
| 恢复动态壁纸原片 | `sudo hisi-wallpaper-reencode --undo` |
| 关掉 ddm 看门狗 | `sudo systemctl disable --now hisi-ddm-watchdog` |
| 卸载补丁自检 | 见 hisi-patch README 的“卸载”一节 |

## 文档

- [docs/gpu.md](docs/gpu.md)：合成器和客户端怎么用上厂商 libmali，mali_buffer_sharing 协议细节，为什么用 glvnd vendor 而不是 LD_PRELOAD
- [docs/known-issues.md](docs/known-issues.md)：ddm 二次登录卡死、动态壁纸、指纹、调试时的注意事项

## 许可

本仓库自己的代码和脚本使用 [MIT](LICENSE) 许可。`libmali`、`wpa_supplicant`、`hciattach`、OpenSSL 1.1 等厂商或第三方组件不在本仓库中，由 `scripts/fetch-uos-blobs.sh` 从你自己机器的 UOS 分区复制，版权归各自所有者。
