#!/bin/bash
# 对比动态壁纸视频在三种路径下的吞吐：纯软件解码 / Mesa llvmpipe 全屏渲染 / 厂商 libmali 全屏渲染
# （libmali 路径依赖已安装的 glvnd vendor 垫片；HISI_MALI_DISABLE=1 让同一程序改走 Mesa）
# 用法：mpvbench.sh [视频] [帧数]   （在桌面用户的会话里运行）
F=${1:-/usr/share/wallpapers/deepin-livewallpapers/Sailfish_In_The_Deep_Sea.mp4}
N=${2:-300}
export XDG_RUNTIME_DIR=/run/user/$(id -u) WAYLAND_DISPLAY=treeland.socket
unset LD_PRELOAD LD_LIBRARY_PATH __EGL_VENDOR_LIBRARY_FILENAMES
COMMON="--no-config --no-audio --untimed --frames=$N --hwdec=no --msg-level=all=warn,vo=info,cplayer=info"
L=${TMPDIR:-/tmp}

run() {
    local name=$1; shift
    local t0 t1
    t0=$(date +%s.%N)
    "$@" >$L/mpvbench-$name.log 2>&1
    local rc=$?
    t1=$(date +%s.%N)
    awk -v n="$N" -v a="$t0" -v b="$t1" -v name="$name" -v rc="$rc" \
        'BEGIN { d = b - a; printf "%-10s rc=%d  %d 帧 %.1f 秒  => %.1f fps\n", name, rc, n, d, n / d }'
    grep -iE 'error|fail|GL_RENDERER|renderer|Mali|llvmpipe|Dropped' $L/mpvbench-$name.log | head -4 | sed 's/^/    /'
}

run decode mpv $COMMON --vo=null "$F"
run llvmpipe env HISI_MALI_DISABLE=1 mpv $COMMON --vo=gpu --gpu-context=wayland --fs -v "$F"
run mali mpv $COMMON --vo=gpu --gpu-context=wayland --fs -v "$F"
