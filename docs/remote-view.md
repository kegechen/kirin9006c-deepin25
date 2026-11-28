# 远程查看 / 控制（tools/remote-view）

在 Windows 浏览器里看 L540 的屏幕并用键鼠操作，类似远程桌面。锁屏界面也能看、能输入；注销后的登录界面（greeter）要输一次 sudo 密码授权（见下文）。

- 采集：treeland 的 wlr-screencopy，走 dmabuf 路径（见下文“原理”）
- 缩放和颜色转换：在 Mali GPU 上直接出 I420，CPU 只读结果（GPU 不可用时自动退回 CPU）
- 编码：x264 软编（ultrafast + zerolatency），只在画面变化时出帧
- 传输：WebSocket，经 SSH 隧道；浏览器用 WebCodecs 解码（Chrome / Edge）
- 输入：浏览器的鼠标键盘事件经 `/dev/uinput` 注入，内核层输入，锁屏也收得到

## 实测

桌面（只有天气动态壁纸在动），`--fps 30`，rv-cap 的 CPU 占用（8 核共 800%），GPU 转换 / `--cpu` 对比：

| 编码尺寸 | GPU 转换（默认） | `--cpu` |
|---|---|---|
| 2160x1440（默认，原分辨率） | 约 89%，29 帧/秒 | 约 143%，28 帧/秒 |
| 1440x960（`--size 1440x960`） | 约 47%，29 帧/秒 | 约 167%，25–27 帧/秒 |
| 1080x720（`--size 1080x720`） | 约 26%，29 帧/秒 | 约 129%，25 帧/秒 |

treeland 两种情况下都是 18–27%。GPU 转换的画面和 grim 截图对比 PSNR 36–37 dB，和 CPU 转换相当（锁屏也一样）。剩下的 CPU 基本都是 x264 编码，所以想再省 CPU 就把编码尺寸调小。

码率和转换方式无关：静止桌面 0.1–0.3 Mbps；1280x800 窗口全幅运动（ffplay testsrc2）约 10 Mbps。

每帧耗时（全分辨率、GPU 转换）：合成器拷贝 16–17 ms、GPU 转换 4 ms（缩放时 8.5 ms，等的是 GPU，不占 CPU）、读出 I420 5 ms、编码 12 ms（单独线程，和下一帧采集并行）。从画面变化到编码完成约 40 ms，再加网络和浏览器解码。点“刷新”补一个关键帧约 120–150 ms，全屏关键帧约 450 KB。

画面静止时不出帧，页面显示“画面静止”。

## 用法

### 1. 把工具放到 L540 上（一次）

```sh
scp -r tools/remote-view <用户>@<L540 IP>:
```

L540 上要有 `gcc make wayland-scanner libwayland-dev libdrm-dev python3-websockets`（deepin 25 装好开发工具后一般都有）；缺 `x264.h` 时 Makefile 会自己 `apt-get download libx264-dev` 解到 `deps/`，不需要 sudo。第一次启动时自动编译 `rv-cap`。

用户要在 `video` 组（打开 `/dev/dri/card0`）和 `input` 组（`/dev/uinput`）里，用 `id` 确认；不在 `input` 组时只能看不能控制。treeland 要加载本仓库的 `libtl-malibuf`（提供 mali_buffer_sharing，装了补丁集就有）。

### 2. 在 Windows 上打开

一键（推荐，会自动把本目录复制到 L540）：

在 PowerShell 里运行（直接双击 `.cmd` 也行，出错时会暂停显示原因）：

```powershell
.\tools\remote-view\rv-go.ps1
```

- 第一个参数是 `用户@L540 IP` 或只写 IP（用户名默认 `uos`），缺省 `uos@192.168.137.27`；后面的参数原样传给服务端，例如 `.\rv-go.ps1 --fps 15` 或 `.\rv-go.ps1 --size 1440x960`。
- 设备上 `~/remote-view` 没有或文件和本机不一致时，自动复制过去；一致就直接启动。首次运行会在设备上编译 `rv-cap`（约 1 分钟），页面等编译完成才打开。
- 全过程有中文输出：SSH 连接耗时、哪些文件变了、复制了多少、设备端日志、就绪后打开浏览器、退出原因。
- 本机端口默认 8765，被占用时自动试 8766 到 8785；也可以先设 `$env:RV_PORT`。
- 设备停在登录界面（注销后）时，会提示输入设备的 sudo 密码，给登录用户加 ACL 后再启动（原因见“原理 / 登录界面”）。密码只经 SSH 的标准输入交给设备上的 `sudo -S`，不进命令行、不落盘；输错会显示 deepin 认证模块给的剩余次数，只剩 1 次时脚本不再尝试，免得把账户锁住。直接回车放弃。
- 看着看着用户注销了、而且授权已失效时，服务端会退出，窗口里提示输密码，授权后自动重启服务端，页面不用关，自己会重连。
- `.\rv-go.ps1 --revoke`：撤销上面的 ACL 后退出（不撤销的话 treeland 重启时也会自动失效）。
- 服从 PowerShell 执行策略；从非交互终端（比如 Claude Code 的工具调用）启动时会自动开一个新的 PowerShell 窗口来跑，不会没反应。
- 关闭窗口或 Ctrl+C 就停；窗口开着时终端保持连接。
- 脚本要存成 **带 BOM 的 UTF-8**，否则 Windows PowerShell 5.1 会按 GBK 读，中文乱码甚至语法出错。

`rv-go.cmd` 只是上面的启动包装（双击或 cmd 里用，参数一样）。`rv-win.cmd` 是老的一键脚本（只启动不复制，浏览器固定延迟 4 秒打开），仍可用：

```bat
tools\remote-view\rv-win.cmd <用户>@<L540 IP>
```

手动：

```sh
ssh -t -L 8765:127.0.0.1:8765 <用户>@<L540 IP> ~/remote-view/run.sh
```

再用 Chrome / Edge 打开 `http://localhost:8765`。**必须用 localhost 打开**：浏览器只在 localhost / HTTPS 下提供 WebCodecs，直接开局域网 IP 不行。

服务端参数（`run.sh` 原样传给 `rv-server.py`）：

| 参数 | 默认 | 说明 |
|---|---|---|
| `--fps N` | 30 | 最大帧率 |
| `--size WxH` | 屏幕物理分辨率 | 编码尺寸，可以只给宽（`1440x`）；只能在原尺寸的 1/3 到 1 倍之间 |
| `--crf N` | 23 | x264 质量，越小越清晰、码率越高 |
| `--threads N` | 4 | x264 线程数 |
| `--no-cursor` | 关 | 画面里不画远端光标 |
| `--cpu` | 关 | 缩放和颜色转换不用 GPU（排查画面问题时对比用）；GPU 初始化失败时会自动这样做，日志里有原因 |
| `--port N` | 8765 | 监听端口（`rv-win.cmd` 用环境变量 `RV_PORT`） |
| `--listen ADDR` | 127.0.0.1 | 改成别的地址会让同网段的人直接连上并控制，不建议 |

### 3. 页面操作

- 鼠标：点击、拖动、右键、中键、滚轮（含横向、高精度滚动）都会转发；坐标按画面比例换算成绝对坐标。
- 键盘：按物理键位转发（`KeyboardEvent.code`），输入法用 L540 上的。先点一下画面让它拿到焦点。
- 工具栏“控制”：取消勾选后只看不操作。
- “锁屏”：发一次 Super+L（依次按下 Super、L，再松开 L、Super）锁定设备屏幕。“控制”取消勾选时不发。想单独发 Super 键，用全屏后的实体 Win 键。
- “刷新”：立即要一个关键帧（花屏时用）。
- “全屏”：全屏后浏览器才会把 Win、Alt+Tab、Ctrl+W 之类的系统键交给页面；长按 Esc 退出全屏。鼠标移到顶边会显示工具栏。
- 设备休眠或熄屏（`wlr-randr` 显示输出 `Enabled: no`）时，treeland 的 wlr-screencopy 会直接 `failed`，连黑屏都抓不到；唤醒屏幕后自动恢复，页面不会一直重连。
- 同一时间只服务一个页面，新开的页面会把旧的顶掉。网络断开后页面每 3 秒自动重连。

## 安全

- 服务端默认只监听 127.0.0.1，只能经 SSH 隧道访问，等于“能 SSH 登录这个用户的人才能看和控制”。
- WebSocket 只接受本机页面（Origin 为 localhost / 127.0.0.1）发起的连接，防止别的网站借浏览器去连本地端口注入输入。
- uinput 输入在内核层，锁屏界面也会收到，所以服务端开着时别人只要能连上就能在锁屏输密码。用完关掉。
- 登录界面的授权只对登录用户本人（ACL `u:<用户>`）开 `/run/treeland` 的 x 和其中 Wayland socket 的 rw，别的用户仍然进不去；不改用户组、不改 `treeland.service`。这个用户本来就能看自己的屏幕、能经 uinput 输入，多出来的只是能连登录界面的合成器。

## 限制

- 只抓第一个输出（L540 只有内屏 eDP-1）；屏幕分辨率或缩放变化时 `rv-cap` 退出，页面 3 秒后重连。
- 没有声音、没有剪贴板同步。
- 登录界面要 sudo 授权，而且 treeland 一重启（重启机器、ddm 重启）授权就没了，要重新输密码。
- 没有硬件编码器可用，编码全靠 CPU：内核里 `CONFIG_VCODEC_VENC` 没开，只有解码节点 `/dev/vcodec_vdec`；`/dev/video0`、`/dev/video1` 是摄像头；`/usr/lib/aarch64-linux-gnu/dri/` 里也没有这颗芯片的 VAAPI 驱动，装 libva / vainfo 没用。嫌占 CPU 可以 `--size 1440x960` 或 `--fps 15`。

## 原理

### 为什么不用 grim / wf-recorder

grim 和 wf-recorder 默认走 screencopy 的 **shm** 路径：合成器把画面从 GPU 内存读回到客户端给的共享内存里。这一步在 treeland 主线程里做，这台机器上全屏一帧要 750–860 ms（只有 1.3 帧/秒），读的时候 treeland 占满一个核，桌面跟着卡。耗时和像素数成正比，抓 540x360 的小区域能到 29 帧/秒，说明瓶颈就是读回本身。

### dmabuf 路径

改成客户端提供 dmabuf，合成器只做一次 GPU 拷贝（12–18 ms），之后由客户端自己从 CPU 读出（13 ms，约 950 MB/s），全屏能到 36 帧/秒，treeland 只占 20–30%。在这台机器上要满足三点，缺一不可：

1. **在 `/dev/dri/card0` 上建 dumb buffer**。`renderD128` 只是显示控制器节点。
2. **先做 DRM 认证再导出 PRIME**。5.4 内核要求未认证的 card0 客户端不能 `PrimeHandleToFD`（报 Permission denied）。做法是 `drmGetMagic` 后用 `mali_buffer_sharing.auth(magic)`（opcode 1）请 treeland 代为 `drmAuthMagic`。协议细节见 [gpu.md](gpu.md)。
3. **用隐式 modifier（`DRM_FORMAT_MOD_INVALID`）建 wl_buffer**。treeland 只对 XR24 宣告了 AFBC（`0x0800000000000071`）和 INVALID，没有 LINEAR。格式用 XR24（内存里是 BGRX）。

采集用 `copy_with_damage`，画面不变时合成器不回帧，自然不出帧；下一帧的请求在本帧读出后立刻发出，和转换、编码并行。

### GPU 转换（gpu-conv.c）

dumb buffer 是写合并内存，CPU 读只有约 950 MB/s：全分辨率 XRGB 一帧 12.4 MB，光读就 13 ms，之后还要在 CPU 上缩放、转换。现在改成在 GPU 上直接画出 I420（每像素 1.5 字节），CPU 要读的数据少了 62%，缩放和转换也不占 CPU：

- 直接 `dlopen` 厂商 libmali（`/usr/local/lib/hisi-mali/libmali.so.0`），用 card0 上的 GBM 平台 EGL + 无表面 GLES2 上下文。glvnd 垫片只认领 Wayland 平台，所以不走系统 libEGL。
- screencopy 的 dumb buffer 当纹理导入；另建一块 dumb buffer 当渲染目标，按 ARGB8888 看待，宽 `out_w/4`、高 `out_h*3/2`，一个像素装 4 个字节：上面 `out_h` 行是 Y，下面左半是 U、右半是 V。片元着色器每个输出字节算一个源矩形的平均（若干个双线性采样点），系数是 BT.709 有限范围，和 CPU 路径一致。
- `glFinish` 之后源缓冲区就能交给合成器写下一帧；读结果时同样用 `DMA_BUF_IOCTL_SYNC` 包住，按行拷进 x264 的三个平面。

libmali 这边踩到的坑：

1. libmali 按 UOS 魔改的 libwayland-client 编译，要 `wl_proxy_ref` / `wl_proxy_unref` / `wl_proxy_get_refcount` 三个上游没有的符号，又是 BIND_NOW，缺了 dlopen 直接失败。只用 GBM 平台时不会真调到，rv-cap 给空实现，Makefile 用 `--export-dynamic-symbol` 导出。
2. libmali 不导出 `eglGetPlatformDisplayEXT`，要用 `eglGetProcAddress` 取。
3. 渲染目标的行距要 64 字节对齐，否则导入 EGLImage 报 `EGL_BAD_ALLOC`（0x3003）。所以目标 dumb buffer 的宽取整到 16 像素，EGLImage 仍按实际宽度；编码宽度要是 8 的倍数。

顺带发现 CPU 路径的一个问题：treeland 发 `ready` 时它的 GPU 拷贝可能还没写完，而 Mali 驱动没把这次拷贝挂成 dma-buf fence，`DMA_BUF_IOCTL_SYNC` 等不到。CPU 从上往下读，会先读到缓冲区里的旧内容。之后的帧旧内容就是上一帧，看不出来；第一帧却是缓冲区里原有的别的画面，顶部有一条错位的杂色带。CPU 路径现在丢掉第一帧。GPU 路径的读取在 GPU 上和 treeland 的拷贝排好了顺序，没有这个问题。

### 登录界面（greeter）

锁屏和登录界面不是一回事：

- **锁屏**：用户会话还在，用户的 treeland socket（`/run/user/<uid>/treeland.socket`）也在，直接能采集。
- **登录界面**：注销后用户会话关了，`/run/user/<uid>` 下没有 Wayland socket。屏幕由 `treeland.service` 以 dde 用户运行的 `treeland --lockscreen` 自己画（greeter 是它进程内的 QML，不是单独的客户端），它的 socket 是 `/run/treeland/wayland-0`。`/run/treeland` 是 systemd 按 `RuntimeDirectory=treeland`、`RuntimeDirectoryMode=0700` 建的，属 dde:dde，别的用户连 stat 都不行。

只把用户加进 dde 组没用：目录是 0700，组没有任何权限。所以授权用 ACL（`/run` 是 tmpfs，内核开了 `CONFIG_TMPFS_POSIX_ACL`）：

```sh
sudo setfacl -m u:uos:rw /run/treeland/wayland-0
sudo setfacl -m u:uos:x  /run/treeland
```

连 unix socket 要对 socket 文件有写权限，所以 socket 要 rw；目录只要 x，不给 r（不能列目录，rv-server 按固定名字 `wayland-0`、`wayland-1` 去找）。treeland 重启时 systemd 删掉重建这个目录，ACL 跟着没了。

采集端代码不用变：登录界面时系统里只有这一个 treeland 进程（dde 用户，也是 DRM master），screencopy 抓的是整个输出，dmabuf 认证（`mali_buffer_sharing.auth`）照样由它代做。用户在 video / render / input 组，`/dev/dri/card0` 和 `/dev/uinput` 不依赖当前活动会话。（2026-11-28：检测、授权提示、中途回到登录界面时服务端退出码这几步已实测；授权后在登录界面上的实际采集还没验证。）

`rv-server.py` 每次页面连上都重新找一遍要采集的 socket：先找用户自己 `XDG_RUNTIME_DIR` 下的 `treeland.socket` / `wayland-0` / `wayland-1`，再找 `/run/treeland/wayland-0`、`wayland-1`；每个都真连一下，排除注销后留下的没人监听的旧 socket。设了 `WAYLAND_DISPLAY` 就只用它。`rv-server.py --probe` 只做这一步：能采集退出 0，在登录界面但没权限退出 3，一个 treeland 都没找到退出 4；服务端运行中遇到“回到登录界面且没权限”也以 3 退出，`rv-go.ps1` 据此请求授权并重启服务端。

### 文件

| 文件 | 说明 |
|---|---|
| `rv-cap.c` | 采集 + 编码，stdout 输出带 8 字节包头的 H.264（Annex B）；stdin 收 `k` 补关键帧，EOF 退出；`-C` 不用 GPU 转换 |
| `gpu-conv.c` | 在 Mali GPU 上缩放 + 转 I420，见上文 |
| `bench.sh` | `./bench.sh [秒数] [rv-cap 参数]`：GPU / CPU 转换各跑一遍，报告帧率、每帧耗时、CPU 占用，解出画面存到 `/tmp/rvbench-*.png`，另存一张 grim 截图用来对比 |
| `rv-server.py` | HTTP + WebSocket 服务端，每个页面起一个 `rv-cap`（起之前找要采集的 treeland，见上文），把浏览器的键鼠事件写进 uinput；`--probe` 只检查能采集哪个 treeland |
| `index.html` | 页面：WebCodecs 解码、画布显示、键鼠转发 |
| `run.sh` | L540 上的启动脚本：编译、停掉残留的旧服务端、启动；退出码 3 / 4 同 `rv-server.py --probe` |
| `rv-go.ps1` | Windows 一键启动（主脚本）：有输出、自动复制、自动开浏览器、自动挑端口、登录界面请求 sudo 授权 |
| `rv-go.cmd` | `rv-go.ps1` 的 cmd / 双击包装（出错暂停） |
| `rv-win.cmd` | Windows 一键启动（老版，只启动不复制） |
| `scbench.c` | screencopy 测速：`./scbench shm\|dumb [帧数] [存最后一帧的 ppm]`，对比 shm 和 dmabuf 两条路径 |
