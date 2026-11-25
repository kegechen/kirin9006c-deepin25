#!/bin/bash
# 比较不同分辨率/编码参数下 mpv 软解 300 帧的 CPU 时间（与系统负载无关，只看本进程用了多少 CPU）
# 用法：decbench.sh <源视频>
src=$1
W=${TMPDIR:-/tmp}/decbench
mkdir -p $W
enc() { # 名字 x264参数...
    local n=$1; shift
    [ -s $W/$n.mp4 ] || nice -n 19 ffmpeg -v error -y -t 10 -i "$src" -an \
        -vf "scale=2560:-2:flags=lanczos" -c:v libx264 -pix_fmt yuv420p "$@" -movflags +faststart $W/$n.mp4
}
enc q1440-crf20 -preset slow -crf 20 -profile:v high
enc q1440-fast-crf20 -preset slow -crf 20 -tune fastdecode -profile:v main

cpu() { # 文件 -> 秒
    local f=$1 out
    out=$( { TIMEFORMAT='%U %S'; time mpv --no-config --really-quiet --vo=null --ao=null --hwdec=no \
        --frames=300 "$f" >/dev/null 2>&1; } 2>&1 )
    awk -v o="$out" 'BEGIN { split(o, a, " "); printf "%.1f", a[1] + a[2] }'
}
printf "%-22s %8s %10s %s\n" 文件 大小MB CPU秒/300帧 码率
for f in "$src" $W/q1440-crf20.mp4 $W/q1440-fast-crf20.mp4; do
    s=$(du -m "$f" | cut -f1)
    br=$(ffprobe -v error -select_streams v:0 -show_entries stream=bit_rate,width,height -of csv=p=0 "$f")
    printf "%-22s %8s %10s %s\n" "$(basename "$f")" "$s" "$(cpu "$f")" "$br"
done
