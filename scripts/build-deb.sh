#!/bin/bash
# 打 deb 包（在 deepin 25 或 CI 的 deepin chroot 里运行）：
#   VERSION=1.2.0 scripts/build-deb.sh [输出目录，默认 dist/]
#   产物：dist/kirin9006c-deepin25_<版本>_<架构>.deb
#
# 内容 = `make install DESTDIR=...` 的全部产物（垫片 .so、hisi-bio-unlock / hisi-phone-link、
# 控制中心插件、rootfs 配置、polkit action、补丁自检工具）。厂商 blob（libmali、指纹库等）
# 不随包分发：装完 deb 后照 README 跑 scripts/fetch-uos-blobs.sh。
#
# 环境：
#   VERSION / ARCH   显式指定版本与架构（CI 用）；默认 git describe / dpkg --print-architecture
#   DCC_LIB_DIR      已解包的 dde-control-center 目录（CI 里没有安装控制中心时给链接器指路）
#   FORCE_MALI_STUB=1 强制用桩链接 libmali-compat（调试用；正常环境有厂商 blob 就用真的）
#
# 说明：没有厂商 libmali.so.0 时，用两符号的桩满足 libmali-compat 的链接（产物的
# NEEDED/未定义符号与真实构建一致，那两个符号运行时由 LD_PRELOAD 的厂商库提供）。
set -euo pipefail

HERE=$(cd "$(dirname "$0")/.."; pwd)
OUT=${1:-$HERE/dist}
PKG=kirin9006c-deepin25
ARCH=${ARCH:-$(dpkg --print-architecture)}

if [ -z "${VERSION:-}" ]; then
    V=$(cd "$HERE" && git describe --tags --always 2>/dev/null || git rev-parse --short HEAD)
    case "$V" in
        v*) VERSION=${V#v} ;;
        *)  VERSION="0.0.0~git$V" ;;
    esac
fi

STAGE=$(mktemp -d /tmp/hisi-deb.XXXXXX)
cleanup() { rm -rf "$STAGE"; }
trap cleanup EXIT

# ---- 1) 编译 ---------------------------------------------------------------
# 厂商 libmali 缺席（CI / 干净机器）时造链接桩，放到 gitignore 的 build/ 里，不会进包
if [ ! -e /usr/local/lib/hisi-mali/libmali.so.0 ] || [ "${FORCE_MALI_STUB:-0}" = 1 ]; then
    MALI_LINK_DIR=$HERE/build/mali-stub
    mkdir -p "$MALI_LINK_DIR"
    cat > "$MALI_LINK_DIR/stub.c" <<'EOF'
/* 构建桩：只为让 libmali-compat.so 链接出与真实构建相同的符号引用。
   运行时这两个符号由 LD_PRELOAD 的厂商 libmali.so.0（全局作用域）提供。 */
void *eglGetProcAddress(const char *name) { (void)name; return 0; }
int gbm_bo_get_fd(void *bo) { (void)bo; return -1; }
EOF
    gcc -O2 -shared -fPIC -Wl,-soname,libmali.so.0 \
        -o "$MALI_LINK_DIR/libmali.so.0" "$MALI_LINK_DIR/stub.c"
else
    MALI_LINK_DIR=/usr/local/lib/hisi-mali
fi

# 控制中心的链接库：环境里已装（deepin 25）就直接用；CI 传 DCC_LIB_DIR 指向解包目录
if [ -n "${DCC_LIB_DIR:-}" ] && ! ls /usr/lib/*/libdde-control-center.so >/dev/null 2>&1; then
    export LIBRARY_PATH="$DCC_LIB_DIR/usr/lib/$(gcc -dumpmachine)${LIBRARY_PATH:+:$LIBRARY_PATH}"
fi

make -C "$HERE" -j"$(nproc)" MALI_LINK_DIR="$MALI_LINK_DIR"
make -C "$HERE" install DESTDIR="$STAGE" NOHINT=1 MALI_LINK_DIR="$MALI_LINK_DIR"

# deb 不切显示管理器（保持系统现状；要 ddm 照 README 手工执行那一步）
rm -f "$STAGE/etc/X11/default-display-manager"
rmdir "$STAGE/etc/X11" 2>/dev/null || true

# ---- 2) DEBIAN 元数据 ------------------------------------------------------
mkdir -p "$STAGE/DEBIAN"

cat > "$STAGE/DEBIAN/control" <<EOF
Package: $PKG
Version: $VERSION
Architecture: $ARCH
Maintainer: kirin9006c-deepin25 <noreply@github.com>
Section: misc
Priority: optional
Installed-Size: $(du -sk "$STAGE" | cut -f1)
Depends: libglib2.0-0, libssl3, libqt6core6, libqt6gui6, libqt6dbus6, libqt6qml6, dde-control-center (>= 6.1), python3
Recommends: qrencode, libqrencode4, grim, qml6-module-qtquick-shapes
Description: deepin 25 patches for Huawei Qingyun L540 (Kirin 9006C)
 麒麟 9006C（华为擎云 L540）deepin 25 补丁集：GPU/Wi-Fi 垫片、锁屏指纹 / 人脸、
 手机指纹解锁（hisi-bio-unlock + hisi-phone-link）、控制中心“手机解锁”插件与补丁自检。
 厂商闭源组件（libmali、指纹 / 蓝牙库）不随本包分发，装完后按仓库 README 运行
 scripts/fetch-uos-blobs.sh 复制；deb 不改默认显示管理器与 /sbin/init。
EOF

# /etc 下的文件都是配置：升级时保留用户修改（dpkg 交互确认），files.list 同理
find "$STAGE/etc" -type f | sed "s|^$STAGE||" | sort > "$STAGE/DEBIAN/conffiles"
echo "/usr/local/lib/hisi-patch/files.list" >> "$STAGE/DEBIAN/conffiles"

write_postinst() {
cat > "$STAGE/DEBIAN/postinst" <<'EOS'
#!/bin/sh
# 尽量幂等、绝不失败：任何一步失败都不阻断安装（|| true）
set -e

if [ "$1" = configure ]; then
    systemd-sysusers >/dev/null 2>&1 || true          # 建 hisi-phone 运行用户
    systemctl daemon-reload >/dev/null 2>&1 || true
    systemctl reload dbus >/dev/null 2>&1 || true     # 让 org.hisi.BioUnlock1 的策略生效

    # 控制中心“生物认证”入口补丁（幂等；插件版本不认识时自动跳过）
    [ -x /usr/local/sbin/hisi-dcc-bio-entry ] && /usr/local/sbin/hisi-dcc-bio-entry || true

    # 只 enable 不 start（避免在安装事务里突然拉起蓝牙广播 / 网络监听）；
    # 正在跑的服务换成新版本文件
    systemctl enable hisi-bio-unlock.service hisi-phone-link.service \
        hisi-ddm-watchdog.service hisi-patch-check.service hisi-patch-check.timer \
        >/dev/null 2>&1 || true
    systemctl try-restart hisi-bio-unlock.service hisi-phone-link.service \
        hisi-patch-check.service >/dev/null 2>&1 || true

    if [ ! -e /usr/local/lib/hisi-mali/libmali.so.0 ]; then
        echo "!! 缺少厂商 libmali.so.0：GPU 加速、treeland 预加载不可用。" >&2
        echo "!! 请先挂载 UOS 根分区并运行 scripts/fetch-uos-blobs.sh（见仓库 README）。" >&2
    fi
    cat <<'EOT'
补丁已安装。剩余步骤（详见仓库 README“安装”一节）：
  - 锁屏界面要 sudo systemctl restart treeland 才换上新 libtl-lockbio（会回到锁屏）
  - 控制中心重开一次才加载“手机解锁”插件；配对：sudo hisi-phone pair
  - 建议重建补丁基线：sudo hisi-patch-check snapshot
EOT
fi
exit 0
EOS
chmod 755 "$STAGE/DEBIAN/postinst"
}

write_postinst

write_preinst() {
cat > "$STAGE/DEBIAN/preinst" <<'EOS'
#!/bin/sh
# /usr/share/deepin-authentication/{allowlist,app-type-list} 属于 deepin-authenticate 包，
# 我们要改它（把 ddm/treeland 加进生物认证调用方白名单），必须先 dpkg-divert 接管路径，
# 否则 dpkg 拒绝覆盖别人的文件。deepin-authenticate 以后升级会把新版本写到 diverted 名下。
set -e
DIVERTED=/usr/share/deepin-authentication
for f in allowlist app-type-list; do
    if ! dpkg-divert --list "$DIVERTED/$f" 2>/dev/null | grep -q .; then
        dpkg-divert --package kirin9006c-deepin25 --add --rename \
            --divert "$DIVERTED/$f.hisi-orig" "$DIVERTED/$f"
    fi
done
exit 0
EOS
chmod 755 "$STAGE/DEBIAN/preinst"
}

write_preinst

cat > "$STAGE/DEBIAN/prerm" <<'EOS'
#!/bin/sh
# 卸载时停服务（升级走 upgrade 分支，不打断正在运行的解锁服务）
set -e
if [ "$1" = remove ] && [ -d /run/systemd/system ]; then
    systemctl disable --now hisi-bio-unlock.service hisi-phone-link.service \
        >/dev/null 2>&1 || true
fi
exit 0
EOS
chmod 755 "$STAGE/DEBIAN/prerm"

cat > "$STAGE/DEBIAN/postrm" <<'EOS'
#!/bin/sh
set -e
if [ "$1" = remove ] || [ "$1" = purge ]; then
    # 还原控制中心“生物认证”入口补丁（幂等）
    [ -x /usr/local/sbin/hisi-dcc-bio-entry ] && /usr/local/sbin/hisi-dcc-bio-entry --undo || true
    # 归还 preinst 里 dpkg-divert 接管的 deepin-authentication 白名单文件：
    # 此时本包的文件已被 dpkg 删除，--rename 把 .hisi-orig（上游版）改回原路径
    for f in /usr/share/deepin-authentication/allowlist /usr/share/deepin-authentication/app-type-list; do
        dpkg-divert --package kirin9006c-deepin25 --remove --rename \
            --divert "$f.hisi-orig" "$f" >/dev/null 2>&1 || true
    done
fi
if [ "$1" = purge ]; then
    # 配对数据（手机公钥等）与补丁基线：用户明确 purge 才删
    rm -rf /var/lib/hisi-bio /var/backups/hisi-patch
    rm -rf /usr/local/lib/hisi-patch/golden /usr/local/lib/hisi-patch/meta
fi
systemctl daemon-reload >/dev/null 2>&1 || true
exit 0
EOS
chmod 755 "$STAGE/DEBIAN/postrm"

# md5sums（debsums 用）
(
    cd "$STAGE"
    find . -type f ! -path './DEBIAN/*' -print0 | sort -z \
        | xargs -0 --no-run-if-empty md5sum | sed 's|\./||' > DEBIAN/md5sums
)

# ---- 3) 打包 ---------------------------------------------------------------
mkdir -p "$OUT"
dpkg-deb --root-owner-group --build "$STAGE" "$OUT/${PKG}_${VERSION}_${ARCH}.deb"
echo "完成：$OUT/${PKG}_${VERSION}_${ARCH}.deb"
