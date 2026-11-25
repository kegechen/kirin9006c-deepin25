#!/bin/bash
# 跑一个图形程序若干秒，报告 CPU 占用和用的 GL 库（系统已装 vendor 垫片，不设任何 EGL 环境变量）
# 用法：cpubench.sh <标签> <秒数> <程序> [参数...]
tag=$1 secs=$2; shift 2
export XDG_RUNTIME_DIR=/run/user/$(id -u) WAYLAND_DISPLAY=treeland.socket
unset LD_PRELOAD LD_LIBRARY_PATH __EGL_VENDOR_LIBRARY_FILENAMES
log=${TMPDIR:-/tmp}/cpubench-$tag.log
"$@" >"$log" 2>&1 &
p=$!
sleep 3
t0=$(awk '{print $14+$15}' /proc/$p/stat 2>/dev/null)
sleep "$secs"
t1=$(awk '{print $14+$15}' /proc/$p/stat 2>/dev/null)
gl=$(grep -oE "libmali\.so|libgallium" /proc/$p/maps 2>/dev/null | sort -u | tr '\n' ' ')
[ -n "$GRAB" ] && grim -s 0.5 "$GRAB"
kill $p 2>/dev/null
wait $p 2>/dev/null
[ -n "$t1" ] && awk -v t="$tag" -v a="$t0" -v b="$t1" -v s="$secs" -v g="$gl" \
    'BEGIN { printf "%-28s CPU %4.0f%%  GL: %s\n", t, (b - a) / s, g }' || { echo "$tag: 进程提前退出"; tail -5 "$log"; }
