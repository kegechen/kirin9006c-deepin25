#!/bin/bash
# 用 glvnd vendor 方式（不设 LD_PRELOAD）跑一个程序几秒，报告用的是哪套 GL 和 CPU
# 用法：vndtest.sh <秒数> <程序> [参数...]
secs=$1; shift
export XDG_RUNTIME_DIR=/run/user/$(id -u) WAYLAND_DISPLAY=treeland.socket HISI_MALI_DEBUG=1
export __EGL_VENDOR_LIBRARY_FILENAMES=${VND_JSON:-/etc/glvnd/egl_vendor.d/10_hisi_mali.json}:/usr/share/glvnd/egl_vendor.d/50_mesa.json
unset LD_PRELOAD LD_LIBRARY_PATH
log=${TMPDIR:-/tmp}/vndtest-$(basename "$1").log
"$@" >"$log" 2>&1 &
p=$!
sleep 2
t0=$(awk '{print $14+$15}' /proc/$p/stat 2>/dev/null)
sleep "$secs"
t1=$(awk '{print $14+$15}' /proc/$p/stat 2>/dev/null)
echo "== pid=$p $*"
grep -oE "/[^ ]*(libmali\.so|libEGL_mesa|libgallium|libEGL_hisi)[^ ]*" /proc/$p/maps 2>/dev/null | sort -u
[ -n "$t1" ] && awk -v a="$t0" -v b="$t1" -v s="$secs" 'BEGIN { printf "CPU %.0f%%\n", (b - a) / s }'
[ -n "$GRAB" ] && grim -s 0.5 "$GRAB"
kill $p 2>/dev/null
wait $p 2>/dev/null
echo "--- 日志（前 25 行）"
head -25 "$log"
