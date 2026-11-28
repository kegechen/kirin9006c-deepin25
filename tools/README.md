# 验证和测速工具

都在桌面用户的会话里运行（脚本会设 `XDG_RUNTIME_DIR=/run/user/$(id -u)`、`WAYLAND_DISPLAY=treeland.socket`），日志写到 `${TMPDIR:-/tmp}`。

| 文件 | 用途 |
|---|---|
| `cpubench.sh <标签> <秒数> <程序> [参数]` | 跑一个图形程序若干秒，报告 CPU 占用和实际加载的 GL 库（libmali / libgallium）。设 `GRAB=文件.png` 时用 grim 截图 |
| `vndtest.sh <秒数> <程序> [参数]` | 同上，但显式指定 glvnd vendor JSON（`VND_JSON=...`）并打开 `HISI_MALI_DEBUG`，用于调试新版垫片 |
| `gdbrun.sh <秒数> <程序> [参数]` | 在 gdb 下用 vendor 方式跑程序，N 秒后发 SIGTERM，抓崩溃栈和所有线程栈 |
| `mpvbench.sh [视频] [帧数]` | 对比纯软解 / Mesa llvmpipe 全屏 / libmali 全屏三种路径的吞吐 |
| `decbench.sh <源视频>` | 比较不同 x264 参数编码后的软解 CPU 时间（mpv 解 300 帧） |
| `decbench2.sh` | 对比每个动态壁纸原片和重编码版的软解开销（需先运行 `hisi-wallpaper-reencode`） |
| `video-preview.qml` | 模拟控制中心壁纸页的视频预览（QtMultimedia），`/usr/lib/qt6/bin/qml video-preview.qml <视频>` |
| `wlegltest.c` | 最小的 libmali Wayland 客户端：eglInitialize + 窗口表面 + 清屏交换若干帧。`gcc -O2 -o wlegltest wlegltest.c -lwayland-client -lwayland-egl -lEGL -lGLESv2` |
| `remote-view/` | 在 Windows 浏览器里远程查看 / 控制 L540 屏幕（含锁屏），screencopy dmabuf 采集 + x264 + WebCodecs，键鼠经 uinput 注入。用法见 [docs/remote-view.md](../docs/remote-view.md) |
| `phone-unlock-test.py` | 不用手机测手机指纹解锁的电脑端：按 [docs/phone-unlock.md](../docs/phone-unlock.md) 的协议扮演 App（Wi‑Fi / TCP，软件密钥）。`sudo hisi-phone pair` 出链接后 `phone-unlock-test.py --host 127.0.0.1 pair '<链接>'`，再 `hello --shot a.jpg` / `unlock` / `bad`（错签名）。需要 python3-cryptography |
| `malitest.c` | 独立验证 libmali 能否按 wlroots 的路子工作（gbm → EGL → dmabuf → EGLImage → FBO → DRM AddFB2）并做简单性能测试。全部 dlopen，`gcc -O2 -o malitest malitest.c -ldl`，`./malitest /usr/local/lib/hisi-mali/libmali.so.0` |

小提示：远程执行 `pkill -f xxx` 时写成 `pkill -f '[x]xx'`，否则会把远端那个命令行里也带 `xxx` 的 `bash -c` 一起杀掉。
