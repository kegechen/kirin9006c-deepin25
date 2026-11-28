#!/bin/bash
# 对比 rv-cap 的 GPU / CPU 转换路径：各跑若干秒，报告帧率、每帧耗时、CPU 占用，并解出最后一帧
# 用法：bench.sh [秒数] [rv-cap 额外参数...]，结果在 ${TMPDIR:-/tmp}/rvbench-{gpu,cpu}.*
set -u
here=$(dirname "$(readlink -f "$0")")
secs=${1:-22}
shift || true
out=${TMPDIR:-/tmp}/rvbench
export XDG_RUNTIME_DIR=/run/user/$(id -u) WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-treeland.socket}

# 去掉 rv-cap 的 8 字节包头，只留 H.264 Annex B
strip() {
	python3 -c '
import struct, sys
d = open(sys.argv[1], "rb").read(); o = open(sys.argv[2], "wb"); i = 0
while i + 8 <= len(d):
    t, f, _, n = struct.unpack(">BBHI", d[i:i + 8]); i += 8
    if t == 2: o.write(d[i:i + n])
    i += n' "$1" "$2"
}

for mode in gpu cpu; do
	flag=; [ $mode = cpu ] && flag=-C
	(sleep "$secs") | "$here/rv-cap" $flag "$@" > "$out-$mode.bin" 2> "$out-$mode.log" &
	sleep $((secs / 2))
	pid=$(pgrep -nf "^$here/rv-cap")
	tl=$(pgrep -x treeland)
	cpu=$(top -b -n 2 -d 3 -p "$pid,$tl" | awk -v p="$pid" -v t="$tl" '$1 == p { a = $9 } $1 == t { b = $9 } END { print a, b }')
	wait
	strip "$out-$mode.bin" "$out-$mode.h264"
	ffmpeg -v error -y -sseof -0.5 -i "$out-$mode.h264" -update 1 -frames:v 1 "$out-$mode.png" 2> /dev/null ||
		ffmpeg -v error -y -i "$out-$mode.h264" -update 1 "$out-$mode.png"
	echo "== $mode: rv-cap / treeland CPU% = $cpu，码流 $(stat -c %s "$out-$mode.h264") 字节"
	grep -v '^rv-cap: 编码' "$out-$mode.log"
done
grim "$out-grim.png"
