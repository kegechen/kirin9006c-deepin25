#!/bin/sh
# 从 UOS 根分区（Roota）复制厂商闭源组件。本仓库不包含任何厂商二进制，它们都来自机器出厂的 UOS。
# 用法（root）：fetch-uos-blobs.sh [UOS 根分区挂载点，默认 /media/uos/Roota]
#   挂载示例：mkdir -p /media/uos/Roota && mount -o ro /dev/disk/by-uuid/<Roota UUID> /media/uos/Roota
set -eu
U=${1:-/media/uos/Roota}
R=${DESTDIR:-}                       # 演练：DESTDIR=/tmp/stage 时只写到临时目录
M=$R/usr/local/lib/hisi-mali

[ "$(id -u)" = 0 ] || [ -n "$R" ] || { echo "需要 root" >&2; exit 2; }
[ -e "$U/usr/lib/aarch64-linux-gnu/libmali.so.0.23.0" ] || {
    echo "在 $U 下找不到 UOS 的 libmali.so.0.23.0（用法：$0 <UOS 根分区挂载点>）" >&2; exit 2; }

put() { # 源 目标 权限：先写临时文件再 rename，正在被进程映射的旧文件不受影响
    install -D -m "$3" "$1" "$(dirname "$2")/.$(basename "$2").new"
    mv -f "$(dirname "$2")/.$(basename "$2").new" "$2"
    echo "  $2"
}

echo "== 厂商 GPU 用户态驱动（Mali-G78 / Valhall r23p0，EGL 1.4 + GLES 3.2 + gbm）"
put "$U/usr/lib/aarch64-linux-gnu/libmali.so.0.23.0" $M/libmali.so.0.23.0 644
ln -sfn libmali.so.0.23.0 $M/libmali.so.0
# treeland 经 LD_LIBRARY_PATH=$M 找 EGL/GLES/gbm 时直接拿到 libmali（不进全局 ld.so 搜索路径）
for n in libEGL.so.1 libgbm.so.1 libGLESv2.so.2; do ln -sfn libmali.so.0 $M/$n; done
# libmali 依赖 UOS 的安全函数库
[ -e /usr/lib/libc_secshared.so ] && [ -z "$R" ] || put "$U/usr/lib/libc_secshared.so" $R/usr/lib/libc_secshared.so 644

echo "== wpa_supplicant 2.9（带 +hisi 补丁，驱动自带 p2p0，不会去建 p2p-dev）及其 OpenSSL 1.1"
put "$U/usr/sbin/wpa_supplicant" $R/usr/local/sbin/wpa_supplicant-hisi 755
put "$U/usr/lib/aarch64-linux-gnu/libssl.so.1.1" $R/usr/local/lib/hisi-wpa/libssl.so.1.1 644
put "$U/usr/lib/aarch64-linux-gnu/libcrypto.so.1.1" $R/usr/local/lib/hisi-wpa/libcrypto.so.1.1 644

echo "== 认 hisi 协议的 hciattach（蓝牙）"
put "$U/usr/bin/hciattach" $R/usr/local/bin/hciattach-hisi 755

echo "完成。下一步：make && sudo make install"
