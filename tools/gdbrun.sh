#!/bin/bash
# 在 gdb 下用 vendor 方式跑程序，N 秒后发 SIGTERM，抓崩溃栈
# 用法：gdbrun.sh <秒数> <程序> [参数...]
secs=$1; shift
export XDG_RUNTIME_DIR=/run/user/$(id -u) WAYLAND_DISPLAY=treeland.socket HISI_MALI_DEBUG=1
export __EGL_VENDOR_LIBRARY_FILENAMES=${VND_JSON:-/etc/glvnd/egl_vendor.d/10_hisi_mali.json}:/usr/share/glvnd/egl_vendor.d/50_mesa.json
unset LD_PRELOAD LD_LIBRARY_PATH
log=${TMPDIR:-/tmp}/gdb-$(basename "$1").log
cat >${TMPDIR:-/tmp}/gdbcmds <<'EOF'
set pagination off
set confirm off
handle SIGTERM nostop noprint pass
handle SIGPIPE nostop noprint pass
handle SIG32 nostop noprint pass
handle SIG33 nostop noprint pass
handle SIG34 nostop noprint pass
handle SIG35 nostop noprint pass
handle SIG36 nostop noprint pass
run
echo \n=== 崩溃现场 ===\n
info registers pc lr sp
bt 30
echo \n=== 所有线程 ===\n
thread apply all bt 8
quit
EOF
gdb -q -batch -x ${TMPDIR:-/tmp}/gdbcmds --args "$@" >"$log" 2>&1 &
g=$!
sleep "$secs"
p=$(pgrep -P $g | head -1)
echo "gdb=$g 子进程=$p"
[ -n "$p" ] && kill -TERM "$p"
for i in $(seq 1 20); do kill -0 $g 2>/dev/null || break; sleep 1; done
kill -0 $g 2>/dev/null && { echo "gdb 未退出，强杀"; kill -9 $g; pkill -9 -P $g; }
grep -vE '^\[(New|Thread .* exited)|^warning: queue|still attached' "$log" | tail -80
