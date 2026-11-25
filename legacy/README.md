# 已弃用：LD_PRELOAD 客户端方案

在 glvnd vendor 垫片（`src/hisi-mali-egl.c`）之前，客户端是这样用上 libmali 的：

| 文件 | 作用 |
|---|---|
| `hisi-mali-user.conf` | 用户服务 drop-in，给 dde-shell、控制中心设 `LD_LIBRARY_PATH` + `LD_PRELOAD=libmali-compat.so libmali.so.0` |
| `mali-exec` | 包装脚本，以符号链接形式放在 `/usr/local/bin/<程序名>`，借 PATH 优先级先于 `/usr/bin/<程序名>` 被找到，设好变量后 exec 真正的程序（用于 mpv、treeland-wallpaper-factory） |
| `install-mali-clients.sh` | 安装上面两样 |

`libmali-compat` 在客户端里先探测合成器有没有 `mali_buffer_sharing`，没有就去掉变量、用 Mesa 重新 exec 自己。

弃用原因：

1. 控制中心经 `deepin-security-loader` 启动，它带 file capability，进程处于 `AT_SECURE` 安全模式，glibc 直接清掉 `LD_PRELOAD` / `LD_LIBRARY_PATH`，drop-in 设了也没用。
2. 每个程序都要单独接入（drop-in 要覆盖 D-Bus 激活和应用管理器两条启动路径，PATH 包装只对按名字启动的程序有效），漏一个就是 llvmpipe。
3. 变量会被子进程继承，需要额外处理。

glvnd vendor 由 libEGL 按 JSON 加载，不受安全模式影响，所有 Wayland 客户端自动生效，所以这套方案已经撤掉。这里的文件只作参考，不要和 vendor 垫片同时安装。
