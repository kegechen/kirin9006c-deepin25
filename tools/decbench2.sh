#!/bin/bash
# 对比每个动态壁纸原片与重编码版的软解开销（mpv 解 300 帧的 CPU 秒数，10 秒视频）
D=/usr/share/wallpapers/deepin-livewallpapers
O=/usr/local/share/hisi-wallpapers/orig
cpu() {
    local out
    out=$( { TIMEFORMAT='%U %S'; time mpv --no-config --really-quiet --vo=null --ao=null --hwdec=no \
        --frames=300 "$1" >/dev/null 2>&1; } 2>&1 )
    awk -v o="$out" 'BEGIN { split(o, a, " "); printf "%.0f%%", (a[1] + a[2]) * 10 }'
}
printf "%-34s %8s %8s %10s\n" 文件 原片 重编码 新码率Mbps
for f in "$D"/*.mp4; do
    b=$(basename "$f")
    br=$(ffprobe -v error -select_streams v:0 -show_entries stream=bit_rate -of csv=p=0 "$f")
    printf "%-34s %8s %8s %10s\n" "$b" "$(cpu "$O/$b")" "$(cpu "$f")" "$(awk -v b="$br" 'BEGIN{printf "%.1f", b/1e6}')"
done
