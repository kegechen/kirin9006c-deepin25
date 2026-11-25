# GPU：让 deepin 25 用上厂商 libmali

deepin 25 的 Mesa 不支持 Mali-G78，默认全程 llvmpipe 软件渲染：treeland 占约 500% CPU，鼠标都卡。UOS 里有厂商闭源的 `libmali.so.0.23.0`（Valhall r23p0-01rel0，EGL 1.4 + GLES 3.2，自带 gbm），它是按 UOS 用户态编译的，放到 deepin 25 上要补三层：

```text
             ┌──────────── Wayland 客户端（dde-shell、控制中心、壁纸进程、mpv …）
             │   libEGL (glvnd) ──► libEGL_hisi_mali.so.0 ──► libmali（认领失败则交给 Mesa）
             │                                   │
             │                       mali_buffer_sharing v5（私有协议，传 dma-buf）
             ▼                                   ▼
treeland ── LD_PRELOAD: libmali-compat + libmali + libtl-malibuf + libtl-norender
             └── wlroots GLES2 渲染器 ── libmali ── /dev/mali0；KMS ── card0（hisi-drm）
```

`libmali.so.0` 放在 `/usr/local/lib/hisi-mali`，**不进全局 ld.so 搜索路径**。同目录下 `libEGL.so.1`、`libGLESv2.so.2`、`libgbm.so.1` 是指向它的符号链接，只给 treeland 的 `LD_LIBRARY_PATH` 用。

## 1. 合成器：treeland

drop-in `treeland.service.d/l540-no-rt.conf` + `l541-malibuf.conf`：

```ini
[Service]
CPUSchedulingPolicy=                      # 厂商内核开了 RT_GROUP_SCHED，SCHED_RR 会 EPERM
Environment=LD_LIBRARY_PATH=/usr/local/lib/hisi-mali
Environment="LD_PRELOAD=.../libmali-compat.so .../libmali.so.0 .../libtl-malibuf.so .../libtl-norender.so"
```

### libtl-norender

- hisi-drm 的 `renderD128` 只是显示控制器，不是 GPU。让 `drmGetRenderDeviceNameFromFd()` 返回 NULL，wlroots 就改用 primary 节点 `card0`（用 DRM master 认证）。
- `drmPrimeHandleToFD()` 导出时补上 `DRM_RDWR`（等于 `O_RDWR`），否则以读写方式 mmap 导入时 EACCES。

### libmali-compat

| 缺口 | 补法 |
|---|---|
| libmali 依赖 UOS 魔改 libwayland-client 的 `wl_proxy_ref` / `unref` / `get_refcount` | 按 libwayland 1.2x 私有布局实现真实引用计数（带运行时校验，失败退回空实现） |
| wlroots 调 `gbm_bo_get_fd_for_plane`，libmali 的 gbm 没有 | 单平面格式等价于 `gbm_bo_get_fd` |
| Qt/waylib 链接 EGL 1.5 的 `eglCreateImage` / `eglDestroyImage`，libmali 只有 KHR 版 | EGLAttrib → EGLint 后转调 KHR 版 |
| wlroots GLES2 渲染器强制要求 `GL_EXT_unpack_subimage` | ES3 里是核心功能，但 Mali 不宣告；ES3+ 时补进扩展串 |
| `LD_PRELOAD`/`LD_LIBRARY_PATH` 被 Xwayland 等子进程继承 | 只在真正的 `/usr/bin/treeland` 进程里把这些变量从环境中删掉（不能更早删，中间的 `env`、`sh` 要靠它们传下去） |

`mali-compat.c` 里还有一段“客户端启动时探测 mali_buffer_sharing，没有就去掉变量、用 Mesa 重新 exec”的逻辑，是给 [legacy/](../legacy/) 里的 LD_PRELOAD 客户端方案用的，现在客户端改走 glvnd 垫片，这段只是不再触发。

## 2. mali_buffer_sharing：libtl-malibuf

libmali 作为 Wayland 客户端时，会在 registry 里找私有协议 `mali_buffer_sharing`，找不到就让 `eglInitialize` 返回 `EGL_NOT_INITIALIZED`（0x3001），一个 global 都不 bind。用 `WAYLAND_DEBUG=1` 看得很清楚。只在客户端伪造这个 global 不行，`wl_registry.bind` 到合成器那里会报协议错误。

协议是公开逆向过的（panfork 里有 XML 和服务端实现），和 r23p0 libmali 内嵌的 `wl_interface` 逐字一致：

```text
event   alloc_device(name: string)      bind 后立刻发，客户端据此 drmOpen(NULL, name)
request create_buffer(id: new_id<wl_buffer>, width, height: int, stride, format, unk1, unk2: uint, fd)
request auth(magic: uint)               转给 drmAuthMagic，没有回复事件
```

实现要点（`src/tl-malibuf.c`）：

- LD_PRELOAD 进 treeland，**不改 treeland 源码**。挂 `wlr_allocator_autocreate` 记下 backend；挂 `wlr_drm_create`，在原函数之后注册 `mali_buffer_sharing` global。所有 `wlr_*` 都在运行时 `dlsym`，不产生 NEEDED 依赖。
- `create_buffer` 照抄 wlroots `types/wlr_drm.c` 的 prime buffer，包成带 `get_dmabuf` 的 `wlr_buffer`，之后的纹理导入走 wlroots 现成的 dmabuf 路径。合成器侧不需要任何 Mali ioctl。
- `alloc_device` 发空串即可。
- `auth` 必须用**当前**的 DRM master fd。启动早期拿到的 fd 后来会被关闭、编号被复用，要扫描 `/proc/self/fd`，用 `drmIsMaster` 找到真正的 master。用错 fd 时客户端的 `DRM_IOCTL_HISI_GEM_CREATE` 会报 EACCES。
- `unk1`/`unk2` 是 modifier 的高/低 32 位，默认 ARM AFBC `0x0800000000000071`。
- 结构体布局对照 wlroots 0.20.2，升级后必须复核。`TL_MALIBUF_DISABLE=1` 关闭，`TL_MALIBUF_DEVICE=<串>` 覆盖 `alloc_device` 的值。

验证：`tools/wlegltest.c` 用 libmali 建窗口表面，60 帧交换约 30 ms。

## 3. 客户端：glvnd EGL vendor 垫片 libEGL_hisi_mali

### 为什么不用 LD_PRELOAD

最早的方案是给 dde-shell、控制中心的用户服务加 drop-in 设 `LD_PRELOAD`，再用 PATH 优先的包装脚本处理 mpv、壁纸进程（见 [legacy/](../legacy/)）。问题是控制中心经 `deepin-security-loader` 启动，后者带 file capability，进程处于 `AT_SECURE` 安全模式，glibc 会直接清掉 `LD_PRELOAD` 和 `LD_LIBRARY_PATH`。

libglvnd 的 libEGL 按 `/etc/glvnd/egl_vendor.d/*.json` 加载 vendor 库，不受安全模式影响，也不需要给任何程序设环境变量。`10_hisi_mali.json` 排在 Mesa 的 `50_mesa.json` 前面，glvnd 会先问本垫片，再问 Mesa。

### 认领规则

- 只认领 **Wayland 平台**的显示，并且合成器必须提供 `mali_buffer_sharing`（另开一个私有事件队列枚举 registry 判断）。
- 程序在黑名单里、`HISI_MALI_DISABLE=1`、`/etc/hisi-mali/egl-off` 存在，或 libmali 初始化失败时返回 `EGL_NO_DISPLAY`，glvnd 自动交给 Mesa。
- GBM、X11、device 平台（Xwayland 的 glamor 等）一律不碰。
- libmali 只在真正认领时才 `dlopen`（`RTLD_LOCAL`；它带 `DT_SYMBOLIC`，内部调用不会被 glvnd 的同名符号截走）。

### 要补的差异

- libmali 的 `wl_egl_window` 是私有的 96 字节结构，和 libwayland-egl 的不兼容。建窗口表面时用 libmali 自己的 `wl_egl_window_create` 另建一个，挂上 libwayland-egl 的 resize/destroy 回调做转发，交换后回填 attached 尺寸。
- `wl_proxy_ref`/`unref`/`get_refcount` 由本库实现，加载 libmali 前把自己提升为 `RTLD_GLOBAL` 供它解析（其余符号都是 hidden）。
- libmali 只有 OpenGL ES。deepin 的 Qt6 是桌面 GL 版，选 config 时把 `EGL_OPENGL_BIT` 换成 `EGL_OPENGL_ES2_BIT`，Qt 会据此改走 GLES；当前绑定的 API 是桌面 OpenGL 时拒绝建上下文，让 GTK4、mpv 自己回退到 GLES。
- EGL 1.5 的 `eglCreateImage`、`eglCreateSync` 等转调 KHR 版本。

### 开关

| 开关 | 作用 |
|---|---|
| `HISI_MALI_DISABLE=1` | 单个程序不用 libmali |
| `/etc/hisi-mali/egl-deny` | 每行一个程序名或完整路径，`#` 开头为注释 |
| `/etc/hisi-mali/egl-off` | 存在时全局不用 |
| `HISI_MALI_DEBUG=1` | 打印认领决策 |

确认某个进程用的是哪套 GL：`grep -oE 'libmali|libgallium|libEGL_hisi' /proc/<pid>/maps | sort -u`。或者用 `tools/vndtest.sh`、`tools/cpubench.sh`。

## 4. 没覆盖到的

- **Xwayland**：glamor 走 GBM 平台，垫片不认领，仍是软件渲染。
- **deepin-terminal、系统监视器**：纯 raster 绘制，本来就不走 EGL。
- **视频解码**：UOS 的 ffmpeg 有 `h264_omx_dec` 能硬解，deepin 的 ffmpeg 6.1 没有 OMX 解码器，只能软解。GPU 只解决了渲染，剩下的开销靠把动态壁纸重编码成 1440p 来降（`hisi-wallpaper-reencode`）：

| 测试 | 4K 原片 | 1440p 重编码 |
|---|---|---|
| mpv 全屏播放（libmali 渲染） | 111% | 42% |
| 控制中心式小窗预览（QtMultimedia） | 90% | 41% |
| 纯解码 300 帧（Sailfish，`tools/decbench2.sh`） | 85% | 36% |

`-tune fastdecode`（CAVLC、关闭去块滤波）在这颗 CPU 上比默认参数再省约 30% 解码时间。
