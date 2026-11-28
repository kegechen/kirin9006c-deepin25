#!/bin/sh
# 在 L540 上启动远程查看服务（默认只监听 127.0.0.1，经 SSH 隧道访问）。
# 一般从 Windows 用 rv-go.ps1 拉起，或者手动：
#   ssh -t -L 8765:127.0.0.1:8765 <用户>@<L540 IP> <本目录>/run.sh
# 然后用 Chrome/Edge 打开 http://localhost:8765 ；断开 SSH 服务就停。
# 参数原样传给 rv-server.py，例如 --size 1440x960 --fps 20
# 采集哪个 treeland（用户会话 / 登录界面）由 rv-server.py 每次浏览器连上时自己找；
# 设了 WAYLAND_DISPLAY 就固定用它。退出码 3：屏幕在登录界面但没权限连，4：找不到 treeland。
set -e
HERE=$(cd "$(dirname "$0")" && pwd)

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"

make -s -C "$HERE" rv-cap

# 上次没退干净的服务端（比如被 nohup 挂在后台）先停掉，免得占着端口
if pkill -f "python3 $HERE/[r]v-server.py"; then
	sleep 1
fi
exec python3 "$HERE/rv-server.py" "$@"
