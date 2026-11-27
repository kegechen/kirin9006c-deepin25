#!/bin/bash
# deepin 25 一键装机：只面向华为擎云 L540-001（麒麟 9006C / UOS 厂商内核 5.4.96-arm64-desktop）。
# 做法：把 deepin 25 的根文件系统装进一个分区，沿用 UOS 的内核、initrd 和厂商驱动；UOS 本身不动。
#
# 在 UOS 下以普通用户运行（脚本会让你输一次 sudo 密码，只从终端读，不落盘）：
#   bash install-deepin25.sh --iso ~/deepin-25-arm64.iso                      # 装到出厂的 Rootb 分区（默认）
#   bash install-deepin25.sh --iso ~/deepin-25-arm64.iso --target /dev/sdb1   # 装到你指定的任意一个分区
#
#   --target rootb          出厂 A/B 布局里的 Rootb 分区（按分区标签自动查找；装过以后重跑也认得，标签保持 Rootb）
#   --target /dev/<分区>    任意一个分区：U 盘、移动硬盘，或内置盘上空出来的分区。必须是分区（lsblk 里 TYPE=part），
#                           不能是整盘；UOS 正在用的（根、/boot、EFI、/data、swap）和已挂载的分区一律拒绝。
#                           分区在 USB 设备上时开机必须插着，而且 deepin 不会被设成“重启后继续进”。
#   --weather-wallpaper <目录>  可选，默认不装：WeatherWallpaper 源码目录（含 deepin-v25/ 和 web/）。给了才会
#                           多一步，在新系统里现场编译天气壁纸（多装几百 MB 的 Qt6/WebEngine）；失败不影响系统。
#   --mirror <URL>          deepin 25 软件源（默认官方社区源）
#
# 目标分区会被 mkfs.ext4 **清空**。动手前显示设备、型号、容量，必须手工输入大写 YES。
# UOS 的根分区、Boot、EFI、/data 不会被格式化。启动菜单只往 UOS Boot 分区的 grub/custom.cfg 加一个入口，
# 而且放在全部装好、自检通过之后的最后一步：中途失败时 UOS 完全没变，只是目标分区已被清空。
# 全部现场编译，不附带任何预编译二进制。sudo 密码绝不写进脚本、文件或日志。
set -uo pipefail
export LC_ALL=C
export PATH="$PATH:/usr/local/sbin:/usr/sbin:/sbin"

TOTAL=12          # 加 --weather-wallpaper 时多一步
STEP=0
ts()       { date +%T; }
next()     { STEP=$((STEP+1)); printf '\n[%s] 【第 %d/%d 步 · 总进度 %d%%】%s\n' "$(ts)" "$STEP" "$TOTAL" $(( (STEP-1)*100/TOTAL )) "$*"; }
progress() { printf '      [%s] %s\n' "$(ts)" "$*"; }
warn()     { printf '      [%s] 警告：%s\n' "$(ts)" "$*"; }
die()      { printf '\n[%s] 【失败】%s\n' "$(ts)" "$*" >&2; exit 1; }
usage()    { sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
S()        { sudo "$@"; }
# 在目标系统里执行（干净环境；IMMUTABLE_DISABLE_REMOUNT 防止 deepin 的 apt 钩子去重挂只读根）
C()        { sudo chroot "$M" /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin HOME=/root LANG=C.UTF-8 \
               IMMUTABLE_DISABLE_REMOUNT=true DEBIAN_FRONTEND=noninteractive "$@"; }
# 耗时命令：输出进日志，每 30 秒报一次“还在跑”和日志最后一行
long() {
  local log=$1 pid n=0 rc
  shift
  "$@" > "$log" 2>&1 &
  pid=$!
  LONG_PID=$pid      # 中断时 cleanup 要先结束它（后台任务不吃 Ctrl-C）
  while kill -0 "$pid" 2>/dev/null; do
    sleep 5
    n=$((n+5))
    if [ $((n % 30)) -eq 0 ]; then
      progress "……仍在进行（已 ${n} 秒）：$(tail -n 1 "$log" 2>/dev/null | tr -d '\r' | cut -c1-90)"
    fi
  done
  wait "$pid"
  rc=$?
  LONG_PID=
  return $rc
}

REPO=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
WORK=/var/lib/deepin25-onekey
M=$WORK/rootfs
ISO_MNT=$WORK/iso
T=
ISO=
TARGET=rootb
DEV_ARG=
WWP_SRC=
WWP_WANT=0
WWP_DONE=0
MIRROR=https://community-packages.deepin.com/beige/
DEV=
PK=
MODE=
REMOVABLE=0
ENTRY=
TITLE=
NEW_UUID=
HAVE_DDM=0
FORMATTED=0
GRUB_DONE=0
SE=
BOOT_REMOUNTED=0
KEEPALIVE=
LONG_PID=
CUSTOM_BAK=
BUILD_DEPS="gcc make pkg-config libwayland-dev libdrm-dev libegl-dev libglvnd-core-dev"
WWP_DEPS="cmake ninja-build g++ pkg-config qt6-base-dev qt6-base-private-dev qt6-declarative-dev qt6-declarative-private-dev qt6-wayland-dev qt6-wayland-private-dev qt6-wayland-dev-tools libdtk6core-dev libmpv-dev treeland-protocols libwayland-dev qml6-module-qtquick-particles qml6-module-qtquick-shapes qt6-webengine-dev qml6-module-qtwebengine"
APT="apt-get -o Acquire::SmartMirrors::Enable=false -o Dpkg::Use-Pty=0"

while [ $# -gt 0 ]; do
  case "$1" in
    --iso)               ISO=${2:?"--iso 需要 ISO 路径"}; shift 2 ;;
    --target)            TARGET=${2:?"--target 需要 rootb 或 /dev/分区"}; shift 2 ;;
    --device)            DEV_ARG=${2:?"--device 需要一个分区"}; shift 2 ;;   # 兼容旧写法 --target usb --device X
    --weather-wallpaper) WWP_SRC=${2:?"--weather-wallpaper 需要源码目录"}; shift 2 ;;
    --mirror)            MIRROR=${2:?"--mirror 需要 URL"}; shift 2 ;;
    -h|--help)           usage ;;
    *)                   die "未知参数：$1（--help 看用法）" ;;
  esac
done
[ -n "$DEV_ARG" ] && TARGET=$DEV_ARG
[ "$TARGET" != usb ] || die "请直接写 --target /dev/分区（例如 --target /dev/sdb1）"
[ "$(id -u)" != 0 ] || die "请用普通用户运行（脚本自己会调用 sudo）"
[ -f "$REPO/Makefile" ] && [ -d "$REPO/src" ] && [ -d "$REPO/rootfs" ] || die "找不到仓库文件：请在 kirin9006c-deepin25 仓库里运行本脚本"
[ -n "$ISO" ] || die "必须用 --iso 指定 deepin 25 arm64 桌面版 ISO（需要你自己下载）"
# 天气壁纸只在明确给了 --weather-wallpaper 时才装，不自动探测
if [ -n "$WWP_SRC" ]; then
  WWP_SRC=$(readlink -f "$WWP_SRC")
  [ -f "$WWP_SRC/deepin-v25/install.sh" ] && [ -d "$WWP_SRC/web" ] || die "$WWP_SRC 里要有 deepin-v25/install.sh 和 web/"
  WWP_WANT=1
  TOTAL=$((TOTAL+1))
fi
# 防重入：第二个实例会把第一个的挂载当成“上次残留”卸掉，第一个就会往 UOS 根分区上写
LOCK=/run/lock/deepin25-onekey.lock
[ -d /run/lock ] && [ -w /run/lock ] || LOCK=/tmp/deepin25-onekey.lock
command -v flock >/dev/null 2>&1 || die "缺少 flock 命令（util-linux 包）"
: >> "$LOCK" 2>/dev/null
exec 8< "$LOCK" || die "打不开锁文件 $LOCK"
flock -n 8 || die "已经有一个装机脚本在运行（锁 $LOCK）。等它结束，或确认它已退出后再运行"

umount_if() {
  mountpoint -q "$1" 2>/dev/null || return 0
  sudo -n umount "$1" 2>/dev/null && return 0
  warn "$1 正忙，改用懒卸载"
  sudo -n umount -l "$1" 2>/dev/null || warn "$1 卸载失败，请手动 sudo umount -l $1"
}
teardown_chroot() {
  mountpoint -q "$M" 2>/dev/null || return 0
  # 结束 chroot 里残留的进程（apt 的 postinst 偶尔会拉起 dbus、gpg-agent），否则卸载不掉
  sudo -n sh -c 'for p in /proc/[0-9]*; do r=$(readlink "$p/root" 2>/dev/null) || continue
    case "$r" in "$1"|"$1"/*) kill "${p#/proc/}" 2>/dev/null ;; esac; done' sh "$M" 2>/dev/null
  sleep 1
  for p in media/uos/Roota home run dev/pts dev sys proc; do umount_if "$M/$p"; done
  return 0
}
cleanup() {
  local rc=$? i
  trap - EXIT
  # 中断时 long 在后台跑的 sudo cp / unsquashfs 不会自己停：先结束它，再卸载
  if [ -n "$LONG_PID" ] && kill -0 "$LONG_PID" 2>/dev/null; then
    printf '\n[%s] 正在结束后台任务……\n' "$(ts)"
    sudo -n pkill -TERM -P "$LONG_PID" 2>/dev/null
    kill "$LONG_PID" 2>/dev/null
    i=0
    while [ $i -lt 10 ]; do
      case "$(ps -o stat= -p "$LONG_PID" 2>/dev/null)" in ''|Z*) break ;; esac
      sleep 1
      i=$((i+1))
    done
  fi
  teardown_chroot
  umount_if "$M"
  umount_if "$ISO_MNT"
  if [ "$SE" = Enforcing ]; then sudo -n setenforce 1 2>/dev/null; fi
  if [ "$BOOT_REMOUNTED" = 1 ]; then sudo -n mount -o remount,ro /boot 2>/dev/null; fi
  if [ $rc -ne 0 ]; then
    printf '\n============================================================\n'
    printf '装机没有完成（停在第 %d 步）。\n' "$STEP"
    if [ -n "$T" ]; then printf '完整日志：%s/install.log；各步骤的详细日志也在 %s/ 下。\n' "$T" "$T"; fi
    if [ "$GRUB_DONE" = 1 ]; then
      printf '启动项已经写进 /boot/grub/custom.cfg。若不想进 deepin：sudo grub-editenv /boot/grub/grubenv unset next_entry\n'
      [ -n "$CUSTOM_BAK" ] && printf '还原启动菜单：sudo cp -a %s /boot/grub/custom.cfg\n' "$CUSTOM_BAK"
    elif [ "$FORMATTED" = 1 ]; then
      printf 'UOS 的系统和启动菜单都没有改动；目标分区 %s 已被格式化（里面是装了一半的 deepin）。\n' "$DEV"
      printf '修好问题后重新运行脚本即可（会再次清空它）。\n'
    else
      printf '还没有动任何分区，UOS 完全没变。\n'
    fi
    printf '============================================================\n'
  fi
  if [ -n "$KEEPALIVE" ]; then kill "$KEEPALIVE" 2>/dev/null; fi
  exit $rc
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP

cat <<EOF

============================================================
 deepin 25 一键装机（华为擎云 L540 / 麒麟 9006C，沿用 UOS 内核）
============================================================
 一共 $TOTAL 步，每步都会打印正在做什么和总进度。
 第 1–2 步只做检查；第 3 步显示要清空的分区并要你输入大写 YES，
 在那之前不会改动任何东西。全程约 30–60 分钟（主要看网速）。

 需要 root 权限：下面 sudo 会提示输入你的登录密码。
 密码只在终端里输入，不会写进任何文件或日志；之后脚本在后台每分钟续一次 sudo 有效期。
 （UOS 没开“开发者模式”时 sudo 会被拒绝，请先在控制中心里开启。）
============================================================
EOF
sudo -v || die "sudo 验证失败。UOS 需要先开启开发者模式才能使用 sudo，开启后重新运行。"
( while sleep 50; do kill -0 $$ 2>/dev/null || exit 0; sudo -n -v 2>/dev/null || exit 0; done ) &
KEEPALIVE=$!

T=$WORK/run-$(date +%Y%m%d-%H%M%S)
S install -d -m 755 "$WORK" || die "建不了工作目录 $WORK"
S install -d -m 700 -o "$(id -u)" -g "$(id -g)" "$T" || die "建不了日志目录 $T"
exec > >(tee -a "$T/install.log") 2>&1
progress "本次运行的日志：$T/install.log"
if mountpoint -q "$M" 2>/dev/null || mountpoint -q "$ISO_MNT" 2>/dev/null; then
  progress "发现上次运行残留的挂载，先清理"
  teardown_chroot
  umount_if "$M"
  umount_if "$ISO_MNT"
fi

# ==== 1 ====
next "检查机器、必需命令和 ISO（只读）"
KVER=$(uname -r)
[ "$KVER" = 5.4.96-arm64-desktop ] || die "内核是 $KVER，不是 5.4.96-arm64-desktop：本脚本只适用于出厂 UOS 的华为擎云 L540"
[ "$(uname -m)" = aarch64 ] || die "不是 arm64 机器"
if tr '\0' '\n' < /proc/device-tree/compatible 2>/dev/null | grep -qx 'hisilicon,baltimore'; then
  progress "SoC：麒麟 9006C（hisilicon,baltimore），内核 $KVER"
else
  warn "设备树里没有 hisilicon,baltimore，可能不是麒麟 9006C；后面复制的厂商驱动未必匹配"
fi
S test -f /boot/grub/grub.cfg || die "找不到 /boot/grub/grub.cfg：请在出厂 UOS 下运行"
miss=
for c in unsquashfs mkfs.ext4 blkid lsblk findmnt mountpoint chroot grub-reboot grub-editenv tar dpkg; do
  command -v "$c" >/dev/null 2>&1 || miss="$miss $c"
done
[ -z "$miss" ] || die "缺少命令：$miss（unsquashfs 在 squashfs-tools 包里：sudo apt install squashfs-tools）"
[ -f "$ISO" ] || die "找不到 ISO：$ISO"
ISO=$(readlink -f "$ISO")
progress "ISO：$ISO（$(du -h "$ISO" | cut -f1)）"
S install -d "$ISO_MNT"
S mount -o loop,ro "$ISO" "$ISO_MNT" || die "挂载 ISO 失败（文件不完整？）"
for f in live/filesystem.squashfs live/filesystem-extra.squashfs; do
  [ -f "$ISO_MNT/$f" ] || die "ISO 里没有 $f：需要 deepin 25 的 arm64 桌面版 ISO"
done
[ -n "$(find "$ISO_MNT/EFI" -maxdepth 2 -iname 'bootaa64.efi' 2>/dev/null)" ] || warn "ISO 里没找到 BOOTAA64.EFI，确认一下是不是 arm64 版"
[ -f "$ISO_MNT/.disk/info" ] && progress "ISO 信息：$(head -c 200 "$ISO_MNT/.disk/info")"
# 格式化之前先确认本机的 unsquashfs 解得开 ISO 的压缩格式（UOS 自带的 squashfs-tools 较旧）
progress "试解压 ISO 里的几个小文件，确认 unsquashfs 支持它的压缩格式"
if ! unsquashfs -n -d "$T/sqtest" "$ISO_MNT/live/filesystem.squashfs" etc/os-release usr/lib/os-release \
       > "$T/sqtest.log" 2>&1 \
   || ! unsquashfs -l "$ISO_MNT/live/filesystem-extra.squashfs" > /dev/null 2>> "$T/sqtest.log"; then
  die "unsquashfs 解不开这个 ISO（多半是 squashfs-tools 太旧、不支持它的压缩格式，见 $T/sqtest.log）"
fi
for f in "$T/sqtest/usr/lib/os-release" "$T/sqtest/etc/os-release"; do
  if [ -f "$f" ] && [ ! -L "$f" ]; then
    progress "ISO 里的系统：$(sed -n 's/^PRETTY_NAME=//p' "$f" | tr -d '"')"
    break
  fi
done
rm -rf "$T/sqtest"
[ -d "/lib/modules/$KVER" ] || die "找不到 /lib/modules/$KVER（UOS 的内核模块），没法给 deepin 用"
[ "$WWP_WANT" = 1 ] && progress "已选装天气壁纸：$WWP_SRC（第 10 步现场编译；不加 --weather-wallpaper 就没有这一步）"
progress "检查通过（ISO 已只读挂载到 $ISO_MNT）"

# ==== 2 ====
next "检查目标分区、UOS 分区布局和启动配置（只读）"
UOS_ROOT=$(readlink -f "$(findmnt -nvo SOURCE /)")
UOS_ROOT_UUID=$(lsblk -dno UUID "$UOS_ROOT" 2>/dev/null | tr -d ' ')
[ -n "$UOS_ROOT_UUID" ] || die "取不到当前根分区 $UOS_ROOT 的 UUID"
BOOT_SRC=$(findmnt -nvo SOURCE /boot 2>/dev/null) || die "/boot 不是单独的分区：出厂 UOS 有独立的 Boot 分区，布局不同，不敢继续"
BOOT_UUID=$(lsblk -dno UUID "$(readlink -f "$BOOT_SRC")" | tr -d ' ')
[ -n "$BOOT_UUID" ] || die "取不到 Boot 分区的 UUID"

# 出厂 Rootb：文件系统标签或 GPT 分区名是 Rootb；或者以前装过 deepin（标签被改过），
# UOS custom.cfg 里的 deepin25-rootb 启动项正指向它——重装时仍按 Rootb 处理（家目录放 /data、启动项 id 不变）
# USB / 可移动盘上的分区一律不算 Rootb（比如 dd 克隆出来的 UOS 盘），按 U 盘处理
is_usb_part() {
  local pk
  pk=/dev/$(lsblk -dno PKNAME "$1" 2>/dev/null | tr -d ' ')
  [ "$(lsblk -dno TRAN "$pk" 2>/dev/null | tr -d ' ')" = usb ] || [ "$(lsblk -dno RM "$pk" 2>/dev/null | tr -d ' ')" = 1 ]
}
is_rootb() {
  local u
  is_usb_part "$1" && return 1
  case " $(lsblk -dno LABEL,PARTLABEL "$1" 2>/dev/null | tr 'A-Z' 'a-z') " in *[[:space:]]rootb[[:space:]]*) return 0 ;; esac
  u=$(lsblk -dno UUID "$1" 2>/dev/null | tr -d ' ')
  [ -n "$u" ] || return 1
  sudo -n awk -v id="'deepin25-rootb'" -v r="root=UUID=$u " '
    /^menuentry / { e = index($0, id) > 0 }
    e && index($0 " ", r) { f = 1 }
    END { exit !f }' /boot/grub/custom.cfg 2>/dev/null
}
if [ "$TARGET" = rootb ]; then
  mapfile -t CAND < <(lsblk -nrpo NAME,TYPE | while read -r n t; do [ "$t" = part ] && is_rootb "$n" && printf '%s\n' "$n"; done)
  if [ ${#CAND[@]} -ne 1 ]; then
    lsblk -o NAME,SIZE,TYPE,FSTYPE,LABEL,PARTLABEL,MOUNTPOINT
    die "按标签找到 ${#CAND[@]} 个 Rootb 分区（要正好 1 个）。请对照上表用 --target /dev/分区 指定"
  fi
  DEV=${CAND[0]}
else
  case "$TARGET" in /dev/*) ;; *) die "--target 只能是 rootb 或 /dev/分区（例如 /dev/sdb1）" ;; esac
  [ -b "$TARGET" ] || die "$TARGET 不是块设备"
  DEV=$(readlink -f "$TARGET")
fi
DEV_TYPE=$(lsblk -dno TYPE "$DEV" | tr -d ' ')
[ "$DEV_TYPE" = part ] || die "$DEV 是 $DEV_TYPE，不是分区：请写分区（如 /dev/sdb1），不要写整盘（如 /dev/sdb）"
if findmnt -rn -S "$DEV" >/dev/null 2>&1; then
  die "$DEV 正挂载在 $(findmnt -rno TARGET -S "$DEV" | head -1)。如果是刚插的 U 盘，先在文件管理器里卸载它（别拔），再运行"
fi
[ "$DEV" != "$UOS_ROOT" ] || die "$DEV 是 UOS 当前的根分区"
while read -r s; do
  [ -n "$s" ] && [ "$(readlink -f "$s")" = "$DEV" ] && die "$DEV 是 UOS 正在用的 swap"
done < <(swapon --show=NAME --noheadings --raw 2>/dev/null)
[ -z "$(ls -A "/sys/class/block/$(basename "$DEV")/holders" 2>/dev/null)" ] || die "$DEV 被 LVM / 加密卷占用"
PT=$(lsblk -dno PARTTYPE "$DEV" | tr -d ' ' | tr 'A-Z' 'a-z')
[ "$PT" != c12a7328-f81f-11d2-ba4b-00a0c93ec93b ] || die "$DEV 是 EFI 系统分区"
LAB=$(lsblk -dno LABEL "$DEV" | sed 's/^ *//; s/ *$//')
case "$(printf '%s' "$LAB" | tr 'A-Z' 'a-z')" in
  roota|boot|backup|_dde_data|efi) die "$DEV 的标签是 $LAB，是 UOS 自己的分区，拒绝" ;;
esac
DEV_UUID=$(lsblk -dno UUID "$DEV" | tr -d ' ')
case "$(lsblk -dno FSTYPE "$DEV" | tr -d ' ')" in
  swap)        die "$DEV 是 swap 分区（UOS 可能用它休眠），拒绝" ;;
  crypto_LUKS) die "$DEV 是加密分区，拒绝" ;;
esac
if [ -n "$DEV_UUID" ] && grep -v '^[[:space:]]*#' /etc/fstab | grep -q "$DEV_UUID"; then
  die "UOS 的 /etc/fstab 里登记了 $DEV（UUID=$DEV_UUID），说明 UOS 在用它"
fi
# fstab / crypttab 里 PARTUUID=、LABEL=、/dev/... 等写法也逐条解析成设备再比
while read -r spec; do
  case "$spec" in
    UUID=*|PARTUUID=*|LABEL=*|PARTLABEL=*) d=$(S findfs "$spec" 2>/dev/null) || continue ;;
    /dev/*) d=$spec ;;
    *) continue ;;
  esac
  [ "$(readlink -f "$d")" = "$DEV" ] && die "UOS 的 /etc/fstab 或 /etc/crypttab 里登记了 $DEV（$spec），说明 UOS 在用它"
done < <(awk '!/^[[:space:]]*#/ && NF {print $1}' /etc/fstab
         S awk '!/^[[:space:]]*#/ && NF {print $2}' /etc/crypttab 2>/dev/null)
PK=/dev/$(lsblk -dno PKNAME "$DEV" | tr -d ' ')
TRAN=$(lsblk -dno TRAN "$PK" 2>/dev/null | tr -d ' ')
RM=$(lsblk -dno RM "$PK" 2>/dev/null | tr -d ' ')
SIZE_B=$(lsblk -bdno SIZE "$DEV" | tr -d ' ')
SIZE_G=$((SIZE_B / 1073741824))
[ "$SIZE_G" -ge 12 ] || die "$DEV 只有 ${SIZE_G} GiB，至少要 12 GiB（建议 20 GiB 以上）"
[ "$SIZE_G" -ge 20 ] || warn "$DEV 只有 ${SIZE_G} GiB，装完基础系统和编译依赖后剩余空间会比较紧"
if is_rootb "$DEV"; then
  MODE=rootb; ENTRY=deepin25-rootb; TITLE="Deepin 25 (Rootb, UOS kernel)"; MODE_DESC="UOS 出厂的 Rootb 分区"
elif [ "$TRAN" = usb ] || [ "$RM" = 1 ]; then
  MODE=part; REMOVABLE=1; ENTRY=deepin25-usb; TITLE="Deepin 25 (USB, UOS kernel)"; MODE_DESC="USB / 可移动设备上的分区"
else
  MODE=part; ENTRY=deepin25-part; TITLE="Deepin 25 ($(basename "$DEV"), UOS kernel)"; MODE_DESC="内置盘上的分区"
fi
progress "目标：$DEV（$MODE_DESC，${SIZE_G} GiB），启动项 id：$ENTRY"
if [ "$TRAN" = usb ]; then
  usb_ok=0
  grep -qE '/(usb-storage|uas)\.ko' "/lib/modules/$KVER/modules.builtin" 2>/dev/null && usb_ok=1
  if [ $usb_ok = 0 ] && command -v lsinitramfs >/dev/null 2>&1; then
    # 先存下清单再 grep：直接 | grep -q 会让 lsinitramfs 吃 SIGPIPE，pipefail 下整条管道算失败
    initrd_list=$(S lsinitramfs "/boot/initrd.img-$KVER" 2>/dev/null || true)
    printf '%s\n' "$initrd_list" | grep -E '/(usb-storage|uas)\.ko' >/dev/null && usb_ok=1
  fi
  [ $usb_ok = 1 ] || die "UOS 的内核和 initrd 里都没有 usb-storage 驱动，从 U 盘启动会找不到根分区。请改用内置盘上的分区"
  progress "UOS 的内核/initrd 带 usb-storage，可以从 USB 设备挂根（这条路径没有实机验证过，属实验性）"
fi
if [ -r /etc/deepin/ab-recovery.json ] && [ -n "$DEV_UUID" ] && grep -qi "$DEV_UUID" /etc/deepin/ab-recovery.json; then
  warn "UOS 的系统备份（/etc/deepin/ab-recovery.json）指向这个分区。装完以后不要在 UOS 里做“系统备份 / 更新前备份”，那会把 deepin 覆盖掉"
fi

# UOS 的启动配置：要能加载 custom.cfg、支持一次性启动（next_entry）；deepin 的启动项照抄 UOS 第一个启动项再改
S grep -q 'custom.cfg' /boot/grub/grub.cfg || die "UOS 的 grub.cfg 不加载 custom.cfg，没法添加启动项"
S grep -q 'next_entry' /boot/grub/grub.cfg || die "UOS 的 grub.cfg 不支持一次性启动（next_entry）"
S awk 'BEGIN{p=0} /^menuentry /{if(!done)p=1} p{print} p&&/^}/{p=0;done=1}' /boot/grub/grub.cfg > "$T/uos-entry.cfg"
[ -s "$T/uos-entry.cfg" ] || die "grub.cfg 里找不到 UOS 的启动项"
grep -q "root=UUID=$UOS_ROOT_UUID" "$T/uos-entry.cfg" || die "UOS 第一个启动项的 root= 不是当前根分区，启动配置和出厂不同，不敢继续"
make_entry() {  # $1 = 新根 UUID
  sed -E \
    -e "1s/^menuentry '[^']*'/menuentry '$TITLE'/" \
    -e "1s/\\\$menuentry_id_option '[^']*'/\$menuentry_id_option '$ENTRY'/" \
    -e "s/root=UUID=$UOS_ROOT_UUID/root=UUID=$1/" \
    -e '/^[[:space:]]*linux[[:space:]]/{s/ splash( |$)/\1/g; s/ (security=selinux|selinux=1)( |$)/\2/g; s/[[:space:]]*$/ selinux=0 plymouth.enable=0 systemd.show_status=1 console=tty0 panic=10 init=\/usr\/local\/sbin\/d25-init/}' \
    -e '/^[[:space:]]*savedefault[[:space:]]*$/d' \
    "$T/uos-entry.cfg"
}
check_entry() {  # $1 = 文件 $2 = UUID
  head -1 "$1" | grep -qF "menuentry '$TITLE'" && grep -qF "'$ENTRY'" "$1" && grep -q "root=UUID=$2 " "$1" \
    && grep -q 'init=/usr/local/sbin/d25-init' "$1"
}
make_entry 00000000-0000-0000-0000-000000000000 > "$T/entry-test.cfg"
check_entry "$T/entry-test.cfg" 00000000-0000-0000-0000-000000000000 || die "从 UOS 启动项改写 deepin 启动项失败（见 $T/entry-test.cfg）"
progress "UOS 启动配置正常：Boot UUID=$BOOT_UUID，启动项模板已生成"

USER_NAME=$(id -un)
USER_UID=$(id -u)
USER_GID=$(id -g)
USER_HASH=$(S getent shadow "$USER_NAME" | cut -d: -f2)
USER_PW=
PW_DESC="密码沿用 UOS 的"
case "$USER_HASH" in
  '$y$'*|'$gy$'*|'$7$'*|'$6$'*|'$5$'*|'$2b$'*|'$2y$'*|'$2a$'*|'$1$'*) ;;
  '$'*)
    # 比如 UOS 开了国密哈希：chpasswd -e 照收不误，但 deepin 的 libxcrypt 验不了，进去以后登录和 sudo 都会失败
    alg=${USER_HASH#?}; alg=${alg%%\$*}
    warn "UOS 的登录密码用的是 deepin 不支持的算法（\$$alg\$），要给 deepin 单独设一个登录密码"
    while :; do
      read -rs -p "      deepin 的登录密码（不显示，不落盘）：" USER_PW < /dev/tty; printf '\n'
      read -rs -p "      再输一次：" pw2 < /dev/tty; printf '\n'
      [ -n "$USER_PW" ] && [ "$USER_PW" = "$pw2" ] && break
      warn "两次不一致或为空，重新输入"
    done
    unset pw2
    USER_HASH=
    PW_DESC="deepin 用刚设的新密码"
    ;;
  *) die "取不到 $USER_NAME 的本地密码哈希（域账号或没设密码？），deepin 里没法沿用同一个密码" ;;
esac
[ "$USER_UID" = 1000 ] || warn "你的 uid 是 $USER_UID，不是 1000：补丁自检的桌面通知默认发给 uid 1000"
DATA_UUID=$(findmnt -no UUID /data 2>/dev/null || true)
DATA_FS=$(findmnt -no FSTYPE /data 2>/dev/null || true)
SWAP_DEV=$(swapon --show=NAME,TYPE --noheadings --raw 2>/dev/null | awk '$2=="partition"{print $1; exit}')
SWAP_UUID=
[ -n "$SWAP_DEV" ] && SWAP_UUID=$(lsblk -dno UUID "$SWAP_DEV" 2>/dev/null | tr -d ' ')
HOSTN="$(hostname)-d25"
TZ_NAME=$(cat /etc/timezone 2>/dev/null || echo Asia/Shanghai)
progress "用户：$USER_NAME（uid $USER_UID，$PW_DESC）；/data：${DATA_UUID:-无}；swap：${SWAP_UUID:-无}"

# ==== 3 ====
next "确认要清空的分区"
printf '\n============================================================\n'
printf ' ⚠️  下面这个分区会被【清空】：mkfs.ext4 重新格式化，原有数据全部丢失、无法恢复\n'
printf '============================================================\n'
lsblk -o NAME,MODEL,TRAN,SIZE,TYPE,FSTYPE,LABEL,MOUNTPOINT "$PK"
printf '\n  要清空的分区：%s（%s）\n' "$DEV" "$MODE_DESC"
printf '  容量：%s GiB    现有文件系统：%s    标签：%s\n' "$SIZE_G" "$(lsblk -dno FSTYPE "$DEV" | tr -d ' ')" "${LAB:-无}"
printf '  所在磁盘：%s    型号：%s    连接：%s\n' "$PK" "$(lsblk -dno MODEL "$PK" | sed 's/ *$//')" "${TRAN:-内置}"
printf '  不会动的：UOS 根分区 %s、Boot、EFI、/data' "$UOS_ROOT"
if [ "$MODE" = rootb ] && [ -n "$DATA_UUID" ]; then printf '（deepin 的家目录会放在 /data/deepin25/home）'; fi
printf '\n  装完后：启动菜单多一个“%s”，下次开机进 deepin；回退方式最后会打印。\n' "$TITLE"
[ "$REMOVABLE" = 1 ] && printf '  USB 设备：每次开机进 deepin 时都要插着；deepin 不会设成“重启后继续进”。\n'
printf '\n请核对型号和容量。确认清空 %s 请输入大写 YES（其它任何输入都取消，什么都不改）：' "$DEV"
# 记下设备身份：确认期间 U 盘被拔插换了，同一个 /dev 名字可能变成另一块盘
dev_id() { printf '%s|%s|%s|%s|%s' "$(lsblk -dno PARTUUID "$DEV" 2>/dev/null)" "$(lsblk -bdno SIZE "$DEV" 2>/dev/null)" \
             "$(lsblk -dno UUID "$DEV" 2>/dev/null)" "$(lsblk -dno SERIAL "$PK" 2>/dev/null)" "$(lsblk -dno MODEL "$PK" 2>/dev/null)"; }
DEV_ID=$(dev_id)
ANSWER=
read -r ANSWER < /dev/tty || true
[ "$ANSWER" = YES ] || { printf '\n已取消，没有改动任何东西。\n'; exit 0; }

# ==== 4 ====
next "⚠️ 格式化 $DEV 并解包 deepin 25 根文件系统（不可撤销；解包约 5–15 分钟，下面有进度条）"
[ -b "$DEV" ] && [ "$(dev_id)" = "$DEV_ID" ] || die "$DEV 在确认期间变了（设备被拔插或换了一块），为安全起见退出，什么都没改。请重新运行"
findmnt -rn -S "$DEV" >/dev/null 2>&1 && die "$DEV 在确认期间被挂载了，先卸载再运行"
# Rootb 保留出厂标签（UOS 和本脚本重跑时都靠它认出 Rootb），其它分区标成 deepin25
NEW_LABEL=deepin25
if [ "$MODE" = rootb ]; then
  case "$(printf '%s' "$LAB" | tr 'A-Z' 'a-z')" in rootb) NEW_LABEL=$LAB ;; *) NEW_LABEL=Rootb ;; esac
fi
S mkfs.ext4 -F -q -L "$NEW_LABEL" "$DEV" || die "mkfs.ext4 失败"
FORMATTED=1
S udevadm settle 2>/dev/null || true
sleep 2
# 桌面可能会自动挂载新文件系统（U 盘尤其常见），先卸掉
findmnt -rno TARGET -S "$DEV" 2>/dev/null | while read -r t; do S umount "$t" 2>/dev/null; done
NEW_UUID=$(S blkid -s UUID -o value "$DEV")
[ -n "$NEW_UUID" ] || die "格式化后取不到 UUID"
S install -d "$M"
S mount "$DEV" "$M" || die "挂载新分区失败"
progress "已格式化（标签 $NEW_LABEL），新 UUID=$NEW_UUID，挂载到 $M"
progress "解包 filesystem.squashfs（基础系统）"
S unsquashfs -f -d "$M" "$ISO_MNT/live/filesystem.squashfs" \
  bin boot dev etc home lib media mnt opt proc root run sbin srv sys tmp usr var 2> "$T/unsquashfs-base.err" \
  || die "解包基础系统失败（$T/unsquashfs-base.err）"
S test -x "$M/usr/lib/systemd/systemd" || die "解包后找不到 systemd"
progress "解包 filesystem-extra.squashfs（桌面组件）"
S unsquashfs -f -d "$M" "$ISO_MNT/live/filesystem-extra.squashfs" \
  etc home media mnt opt root srv usr var 2> "$T/unsquashfs-extra.err" \
  || die "解包桌面组件失败（$T/unsquashfs-extra.err）"
S test -x "$M/usr/bin/ddm" && HAVE_DDM=1
[ "$HAVE_DDM" = 1 ] || warn "ISO 里没有 ddm，第 7 步会联网安装 ddm 和 treeland"
S mkdir -p "$M/persistent" "$M/data" "$M/media/uos/Roota"
S sh -c 'rm -f "$1"/boot/vmlinuz-* "$1"/boot/initrd.img-*' sh "$M"
progress "根文件系统就位：已用 $(df -h --output=used "$M" | tail -1 | tr -d ' ')，剩余 $(df -h --output=avail "$M" | tail -1 | tr -d ' ')"

# ==== 5 ====
next "从 UOS 复制厂商驱动：/vendor 等目录、内核模块、固件、厂商包文件（1–3 GB）"
for d in vendor odm system sec_storage; do
  [ -e "/$d" ] || continue
  progress "复制 /$d"
  S cp -a "/$d" "$M/" || warn "/$d 复制不完整"
done
# 源路径用 /lib/...：UOS 合并 /usr 与否都能用；deepin 25 是合并 /usr 的，目标写 /usr/lib/...
S mkdir -p "$M/usr/lib/modules" "$M/usr/lib/firmware"
long "$T/cp-modules.log" S cp -a "/lib/modules/$KVER" "$M/usr/lib/modules/" || die "复制内核模块失败（$T/cp-modules.log）"
progress "内核模块 $KVER 已复制"
if [ -d /lib/firmware ]; then
  long "$T/cp-firmware.log" S cp -a /lib/firmware/. "$M/usr/lib/firmware/" || warn "固件复制有报错（$T/cp-firmware.log）"
else
  warn "UOS 上没有 /lib/firmware，跳过固件"
fi
progress "固件已复制（同名文件以 UOS 的为准）"
VPKGS="klu-clickpad hw-osal hw-teeossdk hw-govconfig hi-audio hw-audio hw-k-video hw-k-syfingerprint hw-kv-conn"
# shellcheck disable=SC2086
# 用 root 判断文件是否存在：有些厂商文件放在普通用户进不去的目录里
(dpkg -L $VPKGS 2>/dev/null || true) | grep -vE '^/usr/share/(doc|man)/' | sort -u \
  | S sh -c 'while IFS= read -r f; do { [ -f "$f" ] || [ -L "$f" ]; } && printf "%s\n" "${f#/}"; done' \
  > "$T/vendor-files.txt"
progress "厂商包文件：$(wc -l < "$T/vendor-files.txt") 个"
S tar -C / --no-recursion -cpf - -T "$T/vendor-files.txt" 2> "$T/vendor-tar.err" \
  | S tar -C "$M" -xpf - --keep-directory-symlink --skip-old-files 2>> "$T/vendor-tar.err" \
  || warn "厂商文件复制有报错（$T/vendor-tar.err）"
S sh -c 'cd "$1" && while IFS= read -r f; do [ -e "$f" ] || [ -L "$f" ] || printf "%s\n" "$f"; done' sh "$M" \
  < "$T/vendor-files.txt" > "$T/vendor-missing.txt"
if [ -s "$T/vendor-missing.txt" ]; then
  warn "有 $(wc -l < "$T/vendor-missing.txt") 个厂商文件没复制过去（清单 $T/vendor-missing.txt），对应的指纹/音频/TEE 等功能可能不能用"
fi
for l in libssl.so.1.1 libcrypto.so.1.1; do
  S test -e "$M/usr/lib/aarch64-linux-gnu/$l" || S cp -aL "/usr/lib/aarch64-linux-gnu/$l" "$M/usr/lib/aarch64-linux-gnu/"
done
for f in 99-uinput-input-group.rules iflypen-hid.rules 80-udisks2.rules 80-udisks-installer.rules; do
  if [ -e "/etc/udev/rules.d/$f" ] && ! S test -e "$M/etc/udev/rules.d/$f"; then S cp -a "/etc/udev/rules.d/$f" "$M/etc/udev/rules.d/"; fi
done
S mkdir -p "$M/etc/X11/xorg.conf.d"
S tee "$M/etc/X11/xorg.conf.d/20-hisi-modesetting.conf" >/dev/null <<'EOF'
# 来自 UOS（擎云 L540 / hisi-drm）：无 GPU 加速的 modesetting
Section "Device"
    Identifier  "Mali FBDEV"
    Driver      "modesetting"
    Option      "AccelMethod"    "none"
    Option      "UseGPU"         "false"
EndSection
Section "Screen"
    Identifier "Screen0"
    Device     "Mali FBDEV"
    DefaultDepth 24
EndSection
EOF
# 音频：上面只拷了 hw-audio 包的文件，没执行它的 postinst。按 UOS 上 postinst 的结果补齐，否则内置喇叭没声音：
#  1) hwaudioservice 的文件能力 cap_dac_override：没有它就读不了 DMI 表（判机型），也不会给两颗 TFA9874 功放上电。
#     tar 不带文件能力，要重新设。
#  2) postinst 按机型建的两个符号链接（pulse-huawei.conf、audio_vqe_voice.bin），照抄 UOS 的。
#  3) deepin 用 PipeWire：华为的配置集和 path 要放进 ACP 的目录，rootfs 里的 91-pipewire-huawei.rules 让声卡选中它；
#     DMA 周期的修正在 rootfs 的 wireplumber.conf.d/90-hisi-da-combine.conf。
for f in usr/bin/hwaudioservice usr/share/hw-audio/hwaudioservice; do
  S test -f "$M/$f" || continue
  S setcap cap_dac_override=eip "$M/$f" || warn "给 /$f 设文件能力失败，内置喇叭会没声音"
done
for f in usr/share/pulseaudio/alsa-mixer/profile-sets/pulse-huawei.conf usr/share/hw-audio/audio_vqe_voice.bin; do
  if [ -L "/$f" ] && ! S test -e "$M/$f" && ! S test -L "$M/$f"; then
    S cp -a "/$f" "$M/$f" || warn "复制 /$f 失败"
  fi
done
PA=/usr/share/pulseaudio/alsa-mixer
ACP=$M/usr/share/alsa-card-profile/mixer
if [ ! -e "$PA/profile-sets/pulse-huawei.conf" ]; then
  warn "UOS 上没有 $PA/profile-sets/pulse-huawei.conf，PipeWire 用不上华为声卡配置"
elif S install -D -m 644 "$PA/profile-sets/pulse-huawei.conf" "$ACP/profile-sets/pulse-huawei.conf" \
     && S sh -c 'install -d "$2/paths" && for p in "$1"/paths/*-huawei*.conf; do install -m 644 "$p" "$2/paths/" || exit 1; done' \
          sh "$PA" "$ACP"; then
  progress "华为声卡配置已放进 PipeWire（ACP）目录，hwaudioservice 已设 cap_dac_override"
else
  warn "复制华为声卡配置到 $ACP 失败，内置喇叭可能用不上华为配置"
fi
progress "厂商组件复制完成"

# ==== 6 ====
next "写 fstab、主机名、时区、apt 源和网络配置"
HOME_LINE="# 家目录在本分区上"
if [ "$MODE" = rootb ] && [ -n "$DATA_UUID" ]; then
  HOME_LINE="/data/deepin25/home  /home  none  bind,nofail,x-systemd.requires-mounts-for=/data  0 0"
fi
DATA_LINE=
[ -n "$DATA_UUID" ] && DATA_LINE="UUID=$DATA_UUID  /data  ${DATA_FS:-ext4}  defaults,nofail  0 2"
SWAP_LINE=
[ -n "$SWAP_UUID" ] && SWAP_LINE="UUID=$SWAP_UUID  none  swap  sw,nofail  0 0"
S tee "$M/etc/fstab" >/dev/null <<EOF
# deepin 25（$MODE_DESC，UUID=$NEW_UUID），沿用 UOS 厂商内核 $KVER；/boot 故意不挂载（UOS 的 Boot 分区不让 deepin 改）
UUID=$NEW_UUID  /  ext4  rw,relatime,errors=remount-ro  0 1
$DATA_LINE
$HOME_LINE
$SWAP_LINE
EOF
printf '%s\n' "$HOSTN" | S tee "$M/etc/hostname" >/dev/null
printf '127.0.0.1\tlocalhost\n127.0.1.1\t%s\n::1\tlocalhost ip6-localhost ip6-loopback\n' "$HOSTN" | S tee "$M/etc/hosts" >/dev/null
S sh -c 'tr -d "-" < /proc/sys/kernel/random/uuid > "$1/etc/machine-id"' sh "$M"
S ln -sf "/usr/share/zoneinfo/$TZ_NAME" "$M/etc/localtime"
printf '%s\n' "$TZ_NAME" | S tee "$M/etc/timezone" >/dev/null
for f in default/keyboard adjtime; do
  if [ -f "/etc/$f" ]; then S cp -a "/etc/$f" "$M/etc/$f"; fi
done
printf 'deb %s crimson main commercial community\n' "$MIRROR" | S tee "$M/etc/apt/sources.list" >/dev/null
S tee "$M/etc/apt/preferences.d/90-l540-keep-vendor-boot" >/dev/null <<'EOF'
# 只能用 UOS 厂商内核启动（不挂 /boot）；禁止 deepin 内核与引导包升级
Package: linux-image-* linux-headers-* linux-kbuild-* linux-modules-* grub-* grub2* shim* shim-signed*
Pin: release *
Pin-Priority: -1
EOF
S mkdir -p "$M/etc/NetworkManager/system-connections" "$M/etc/NetworkManager/conf.d"
S cp -a /etc/NetworkManager/system-connections/. "$M/etc/NetworkManager/system-connections/" 2>/dev/null || true
progress "已带上 UOS 里保存的网络连接：$(S ls /etc/NetworkManager/system-connections 2>/dev/null | wc -l) 个"
for u in deepin-installer.service deepin-installer-first-boot.service deepin-installer-extra.service live-config.service \
         deepin-immutable-cleanup.timer deepin-immutable-cleanup.service; do
  S ln -sf /dev/null "$M/etc/systemd/system/$u"
done
S rm -f "$M/etc/systemd/system/timers.target.wants/deepin-immutable-cleanup.timer"
S mkdir -p "$M/var/log/journal"
progress "基础配置写入完成"

# ==== 7 ====
next "chroot：建用户、生成中文 locale、联网安装依赖（可能 10–30 分钟）"
SE=$(getenforce 2>/dev/null || true)
if [ "$SE" = Enforcing ]; then S setenforce 0; progress "SELinux 临时切到 permissive（脚本结束时自动恢复）"; fi
# 每个 bind 都改成 slave：UOS 的挂载默认是 shared，不改的话在 $M/dev 里挂 dev/pts、之后再卸载，
# 都会传播回 UOS 自己的 /dev
for d in proc sys dev dev/pts; do
  S mount --bind "/$d" "$M/$d" || die "绑定 /$d 失败"
  S mount --make-slave "$M/$d" || die "把 $M/$d 设成 slave 失败"
done
S mount -t tmpfs tmpfs "$M/run" || die "挂 /run 失败"
if [ "$MODE" = rootb ] && [ -n "$DATA_UUID" ]; then
  S mkdir -p /data/deepin25/home
  S mount --bind /data/deepin25/home "$M/home" || die "绑定 /data/deepin25/home 失败"
  S mount --make-slave "$M/home" || die "把 $M/home 设成 slave 失败"
fi
# resolv.conf 在 deepin 里通常是指向 /run 的符号链接：先原样备份（cp -a 不跟随链接），删掉再写普通文件，
# 否则写入会顺着链接落到 UOS 自己的文件上
S cp -a "$M/etc/resolv.conf" "$T/resolv.conf.iso" 2>/dev/null || true
S rm -f "$M/etc/resolv.conf"
S sh -c 'cat /etc/resolv.conf > "$1/etc/resolv.conf"' sh "$M"
printf '#!/bin/sh\nexit 101\n' | S tee "$M/usr/sbin/policy-rc.d" >/dev/null
S chmod 755 "$M/usr/sbin/policy-rc.d"
C ldconfig
S sed -i -E 's/^# *(zh_CN\.UTF-8 UTF-8)/\1/; s/^# *(en_US\.UTF-8 UTF-8)/\1/' "$M/etc/locale.gen" 2>/dev/null || true
C locale-gen > "$T/locale-gen.log" 2>&1 || warn "locale-gen 有报错（$T/locale-gen.log）"
printf 'LANG=zh_CN.UTF-8\nLANGUAGE=zh_CN\n' | S tee "$M/etc/default/locale" >/dev/null
EXIST=$(C getent passwd "$USER_UID" | cut -d: -f1)
[ -z "$EXIST" ] || [ "$EXIST" = "$USER_NAME" ] || die "ISO 里已有 uid $USER_UID 的用户 $EXIST，和你的用户名冲突"
C getent group "$USER_GID" >/dev/null || C groupadd -g "$USER_GID" "$USER_NAME" || die "建用户组失败"
UGROUPS=$(C sh -c 'for g in sudo adm audio video render input netdev lpadmin plugdev users bluetooth lp scanner; do
  getent group "$g" >/dev/null && printf "%s," "$g"; done')
UGROUPS=${UGROUPS%,}
if [ -z "$EXIST" ]; then
  C useradd -u "$USER_UID" -g "$USER_GID" -M -d "/home/$USER_NAME" -s /bin/bash -G "$UGROUPS" "$USER_NAME" || die "建用户失败"
else
  C usermod -a -G "$UGROUPS" "$USER_NAME" || die "设置用户组失败"
fi
# 密码（哈希）走 stdin，不出现在命令行（ps 看得到命令行）；printf 是内建命令，不产生进程
if [ -n "$USER_HASH" ]; then
  printf '%s:%s\n' "$USER_NAME" "$USER_HASH" | C chpasswd -e || die "设置密码失败"
else
  printf '%s:%s\n' "$USER_NAME" "$USER_PW" | C chpasswd || die "设置密码失败"
fi
unset USER_HASH USER_PW
S mkdir -p "$M/home/$USER_NAME"
if [ -z "$(S ls -A "$M/home/$USER_NAME" 2>/dev/null)" ]; then
  S cp -a "$M/etc/skel/." "$M/home/$USER_NAME/"
else
  progress "家目录已有内容（以前装过），原样保留"
fi
S chown -R "$USER_UID:$USER_GID" "$M/home/$USER_NAME"
progress "用户 $USER_NAME 已建好（组：$UGROUPS）"
progress "apt update（$MIRROR）"
# shellcheck disable=SC2086
long "$T/apt-update.log" C $APT update || die "apt update 失败，检查网络（$T/apt-update.log）"
NEED=$BUILD_DEPS
[ "$HAVE_DDM" = 1 ] || NEED="$NEED ddm treeland"
progress "安装编译依赖：$NEED（详细输出 $T/apt-install.log，可另开终端 tail -f 查看）"
# shellcheck disable=SC2086
long "$T/apt-install.log" C $APT install -y --no-install-recommends $NEED || die "依赖装不上（$T/apt-install.log）"
S test -x "$M/usr/bin/ddm" || die "装完依赖后仍没有 ddm"
if [ -n "$WWP_SRC" ]; then
  progress "安装天气壁纸的编译依赖（Qt6 / WebEngine，几百 MB）"
  # shellcheck disable=SC2086
  long "$T/apt-wwp.log" C $APT install -y --no-install-recommends $WWP_DEPS \
    || { warn "天气壁纸依赖装不全，跳过天气壁纸（$T/apt-wwp.log）"; WWP_SRC=; }
fi
progress "chroot 初始化完成"

# ==== 8 ====
next "装首启必需补丁：d25-init（关 elfverify）、改写 /sbin/init、切到 ddm+treeland、开机兜底"
S install -D -m 755 "$REPO/rootfs/usr/local/sbin/d25-init" "$M/usr/local/sbin/d25-init" || die "装 d25-init 失败"
# UOS 的 initrd 只在新根 /boot/grub/grub.cfg 含 password_pbkdf2 时才认 init=，所以直接把 /sbin/init 换成 d25-init
# divert 要挂在 dpkg 数据库里登记的那个路径上（/usr/sbin/init 或 /sbin/init），否则 systemd-sysv 升级会从别名路径覆盖掉
INIT_PATH=/usr/sbin/init
INIT_ALIAS=/sbin/init
if ! C dpkg-query -S /usr/sbin/init >/dev/null 2>&1 && C dpkg-query -S /sbin/init >/dev/null 2>&1; then
  INIT_PATH=/sbin/init
  INIT_ALIAS=/usr/sbin/init
fi
C dpkg-divert --local --rename --divert /usr/sbin/init.systemd --add "$INIT_PATH" > "$T/divert.log" 2>&1 || die "dpkg-divert 失败（$T/divert.log）"
# 另一个别名也登记上（只登记不改名）：以后 systemd-sysv 换了路径，新文件也会被挪开
C dpkg-divert --local --no-rename --divert /usr/sbin/init.systemd-alias --add "$INIT_ALIAS" >> "$T/divert.log" 2>&1 \
  || warn "给 $INIT_ALIAS 登记 divert 失败（$T/divert.log），不影响开机"
S ln -sfn ../local/sbin/d25-init "$M/usr/sbin/init" || die "改写 /usr/sbin/init 失败"
progress "/sbin/init 已换成 d25-init（dpkg 登记路径：$INIT_PATH）"
printf '/usr/bin/ddm\n' | S tee "$M/etc/X11/default-display-manager" >/dev/null
C systemctl disable lightdm.service >/dev/null 2>&1 || true
C systemctl enable ddm.service >/dev/null 2>&1 || die "启用 ddm 失败"
# 厂商服务照 UOS 的启用状态来
for u in 110x oam_hisi teecd hwgovconfig hciattach hwfp_init; do
  st=$(systemctl is-enabled "$u.service" 2>/dev/null || true)
  if [ "$st" = enabled ]; then
    if C systemctl enable "$u.service" >/dev/null 2>&1; then progress "启用厂商服务 $u"; else warn "启用 $u 失败"; fi
  fi
done
# 指纹：hwfp_init.sh 写死了 /usr/bin/fingerprint_daemon
FPL=$M/usr/bin/fingerprint_daemon
if ! S test -e "$FPL" && ! S test -L "$FPL"; then
  r=$(readlink -f /usr/bin/fingerprint_daemon 2>/dev/null || true)
  if [ -n "$r" ] && [ -x "$r" ]; then
    if [ "$r" != /usr/bin/fingerprint_daemon ] && S test -e "$M$r"; then S ln -s "$r" "$FPL"; else S cp -aL /usr/bin/fingerprint_daemon "$FPL"; fi
  fi
fi
# 开机兜底：10 分钟内登录界面（ddm）没起来就抓日志并重启。grub 的一次性启动项这次开机已经用掉，重启就回 UOS
S tee "$M/usr/local/sbin/d25-bootguard" >/dev/null <<'EOF'
#!/bin/bash
# deepin 开机兜底：10 分钟内 ddm（登录界面）没起来，就抓日志并重启（回到 UOS）。
# 不想要：sudo systemctl disable d25-bootguard
D=/var/log/d25-bootguard
mountpoint -q /data && D=/data/deepin25/boot-logs
mkdir -p "$D"
B=$(date +%Y%m%d-%H%M%S)
for i in $(seq 1 60); do
  if systemctl is-active --quiet ddm.service; then echo "ddm ok after ~$((i*10))s" > "$D/$B-ok.txt"; exit 0; fi
  sleep 10
done
echo "10 分钟内 ddm 没起来，重启回 UOS" > "$D/$B-reboot.txt"
journalctl -b --no-pager > "$D/$B-journal.txt" 2>&1
dmesg > "$D/$B-dmesg.txt" 2>&1
{ systemctl --failed --no-pager; ps -eo pid,stat,etime,cmd; } > "$D/$B-state.txt" 2>&1
sync
systemctl reboot
EOF
S chmod 755 "$M/usr/local/sbin/d25-bootguard"
S tee "$M/etc/systemd/system/d25-bootguard.service" >/dev/null <<'EOF'
[Unit]
Description=deepin 25 boot guard: reboot back to UOS if ddm is not up within 10 minutes
After=local-fs.target

[Service]
Type=simple
ExecStart=/usr/local/sbin/d25-bootguard

[Install]
WantedBy=multi-user.target
EOF
# 补丁自检 / hisi-next-boot 读这里的 UOS Boot 分区和启动项 id；sha256 由首次开机的 snapshot 填
S install -d "$M/usr/local/lib/hisi-patch/meta"
printf 'uuid=%s\npath=/grub/custom.cfg\nentry=%s\nsha256=\n' "$BOOT_UUID" "$ENTRY" | S tee "$M/usr/local/lib/hisi-patch/meta/boot-custom.meta" >/dev/null
if [ "$REMOVABLE" = 1 ]; then
  S touch "$M/etc/hisi-next-boot.off"
  progress "USB 设备：已关闭“重启后继续进 deepin”（/etc/hisi-next-boot.off），拔掉后开机直接是 UOS"
fi
progress "首启补丁已就位"

# ==== 9 ====
next "现场编译并安装补丁（5 个 .so 垫片 + 自检、看门狗、启动切换脚本）"
S mount --bind / "$M/media/uos/Roota" || die "绑定 UOS 根分区失败"
S mount --make-slave "$M/media/uos/Roota" || die "把 UOS 根分区的绑定设成 slave 失败"
S mount -o remount,bind,ro "$M/media/uos/Roota" || die "UOS 根分区改只读失败"
S install -d "$M/opt/kirin9006c-deepin25"
tar -C "$REPO" --exclude=./build --exclude=./.git -cf - . | S tar -C "$M/opt/kirin9006c-deepin25" -xf - --no-same-owner \
  || die "复制仓库源码失败"
progress "从 UOS 复制厂商 libmali / wpa_supplicant / hciattach"
C sh /opt/kirin9006c-deepin25/scripts/fetch-uos-blobs.sh /media/uos/Roota > "$T/fetch-blobs.log" 2>&1 \
  || die "复制厂商组件失败（$T/fetch-blobs.log）"
umount_if "$M/media/uos/Roota"
C make -C /opt/kirin9006c-deepin25 clean > /dev/null 2>&1
progress "make：编译 5 个垫片"
long "$T/make.log" C make -C /opt/kirin9006c-deepin25 || die "编译失败（$T/make.log）"
C make -C /opt/kirin9006c-deepin25 install NOHINT=1 > "$T/make-install.log" 2>&1 || die "make install 失败（$T/make-install.log）"
for u in hisi-patch-firstboot.service hisi-patch-check.service hisi-patch-check.timer hisi-ddm-watchdog.service \
         hisi-next-boot.timer d25-bootguard.service; do
  C systemctl enable "$u" >/dev/null 2>&1 || warn "启用 $u 失败"
done
progress "补丁已编译安装（源码留在新系统的 /opt/kirin9006c-deepin25；基线在首次开机时自动建立）"

# ==== 可选：天气壁纸（只在给了 --weather-wallpaper 时出现，默认没有这一步）====
if [ "$WWP_WANT" = 1 ]; then
  next "现场编译天气壁纸（可选，失败不影响系统）"
  if [ -z "$WWP_SRC" ]; then
    progress "跳过：第 7 步没装上天气壁纸的依赖（$T/apt-wwp.log）"
  elif ! { S install -d "$M/opt/WeatherWallpaper" \
           && tar -C "$WWP_SRC" --exclude=deepin-v25/build --exclude=deepin-v25/third_party -cf - deepin-v25 web \
              | S tar -C "$M/opt/WeatherWallpaper" -xf - --no-same-owner; }; then
    warn "复制天气壁纸源码失败，跳过"
  else
    S chown -R "$USER_UID:$USER_GID" "$M/opt/WeatherWallpaper"
    progress "编译 factory 并离线渲染壁纸预览（几分钟）"
    # chroot 里建不了 user namespace（Chromium 沙箱起不来），/dev/shm 也不是 tmpfs：离线渲染时关沙箱、共享内存改走 /tmp
    if long "$T/wwp-build.log" C runuser -u "$USER_NAME" -- env HOME="/home/$USER_NAME" QT_QPA_PLATFORM=offscreen \
         QTWEBENGINE_DISABLE_SANDBOX=1 QTWEBENGINE_CHROMIUM_FLAGS='--no-sandbox --disable-dev-shm-usage' \
         timeout 1800 sh /opt/WeatherWallpaper/deepin-v25/install.sh build \
       && C sh /opt/WeatherWallpaper/deepin-v25/install.sh install > "$T/wwp-install.log" 2>&1; then
      progress "天气壁纸已安装（进 deepin 后在控制中心 → 壁纸 → 动态壁纸里选）"
      WWP_DONE=1
    else
      warn "天气壁纸没装成（不影响系统）。进 deepin 后可手动：cd /opt/WeatherWallpaper/deepin-v25 && ./install.sh build && sudo ./install.sh install（日志 $T/wwp-*.log）"
    fi
  fi
fi

# ==== 10 ====
next "自检关键文件，卸载新系统"
fail=
[ "$(S readlink "$M/usr/sbin/init")" = ../local/sbin/d25-init ] || fail="$fail /usr/sbin/init"
S test -e "$M/usr/sbin/init.systemd" || fail="$fail /usr/sbin/init.systemd"
[ "$(C dpkg-divert --truename "$INIT_PATH" 2>/dev/null)" = /usr/sbin/init.systemd ] || fail="$fail divert($INIT_PATH)"
S test -x "$M/usr/local/sbin/d25-init" || fail="$fail d25-init"
for f in usr/bin/ddm usr/local/lib/hisi-mali/libmali.so.0 usr/local/lib/hisi-mali/libEGL_hisi_mali.so.0 \
         usr/local/lib/hisi-mali/libmali-compat.so usr/local/lib/hisi-mali/libtl-malibuf.so \
         usr/local/lib/hisi-treeland/libtl-norender.so usr/local/lib/hisi-nmfix/libnm-station-fix.so \
         usr/local/sbin/wpa_supplicant-hisi etc/glvnd/egl_vendor.d/10_hisi_mali.json \
         "usr/lib/modules/$KVER/modules.dep"; do
  S test -e "$M/$f" || fail="$fail /$f"
done
S grep -q "^UUID=${NEW_UUID}[[:space:]]\+/[[:space:]]" "$M/etc/fstab" || fail="$fail fstab"
S awk -F: -v u="$USER_NAME" '$1==u && substr($2,1,1)=="$" {f=1} END{exit !f}' "$M/etc/shadow" || fail="$fail 用户密码"
S awk -F: -v u="$USER_NAME" '$1=="sudo" && ("," $4 ",") ~ ("," u ",") {f=1} END{exit !f}' "$M/etc/group" || fail="$fail sudo组"
# 声音不影响开机，只警告
if S test -f "$M/usr/bin/hwaudioservice" && ! S getcap "$M/usr/bin/hwaudioservice" 2>/dev/null | grep -q cap_dac_override; then
  warn "hwaudioservice 没有 cap_dac_override，内置喇叭会没声音（见 docs/known-issues.md 的“声音”一节）"
fi
for f in etc/udev/rules.d/91-pipewire-huawei.rules etc/wireplumber/wireplumber.conf.d/90-hisi-da-combine.conf \
         usr/share/alsa-card-profile/mixer/profile-sets/pulse-huawei.conf; do
  S test -e "$M/$f" || warn "缺 /$f，内置喇叭可能没声音或有杂音"
done
# 还原 ISO 原来的 resolv.conf 和 policy-rc.d
S rm -f "$M/usr/sbin/policy-rc.d" "$M/etc/resolv.conf"
if S test -e "$T/resolv.conf.iso" || S test -L "$T/resolv.conf.iso"; then S cp -a "$T/resolv.conf.iso" "$M/etc/resolv.conf"; fi
[ -z "$fail" ] || die "自检没通过：$fail（启动菜单还没写，UOS 没有改动）"
progress "自检通过"
teardown_chroot
S sync
umount_if "$M"
umount_if "$ISO_MNT"
progress "新系统已卸载"

# ==== 11 ====
next "在 UOS Boot 分区的 grub/custom.cfg 添加启动项 $ENTRY（不改默认项，不跑 update-grub）"
BG=/boot/grub
if ! findmnt -no OPTIONS /boot | tr ',' '\n' | grep -qx rw; then
  S mount -o remount,rw /boot || die "/boot 是只读的，改读写失败"
  BOOT_REMOUNTED=1
fi
if S test -f "$BG/custom.cfg"; then
  CUSTOM_BAK=$BG/custom.cfg.bak-$(date +%Y%m%d-%H%M%S)
  S cp -a "$BG/custom.cfg" "$CUSTOM_BAK" || die "备份 custom.cfg 失败"
  progress "已备份原来的 custom.cfg → $CUSTOM_BAK"
fi
make_entry "$NEW_UUID" > "$T/deepin25-entry.cfg"
check_entry "$T/deepin25-entry.cfg" "$NEW_UUID" || die "生成启动项失败（$T/deepin25-entry.cfg）"
# 去掉同 id 的旧入口（重装后 UUID 变了），再追加新的
{
  S cat "$BG/custom.cfg" 2>/dev/null | awk -v id="'$ENTRY'" '
    skip==0 && /^menuentry / && index($0, id) {skip=1; next}
    skip==1 { if ($0 ~ /^}/) skip=0; next }
    {print}'
  cat "$T/deepin25-entry.cfg"
} > "$T/custom.cfg.new"
S install -m 644 "$T/custom.cfg.new" "$BG/.custom.cfg.new" || die "写 custom.cfg 失败"
S mv -f "$BG/.custom.cfg.new" "$BG/custom.cfg" || die "写 custom.cfg 失败"
GRUB_DONE=1
S grub-reboot "$ENTRY" || die "grub-reboot 失败"
S grub-editenv "$BG/grubenv" list | grep -qx "next_entry=$ENTRY" || die "grubenv 里没有 next_entry=$ENTRY"
S sync
progress "启动项已添加，下次开机进 deepin（只这一次；grub 默认项仍是 UOS）"

# ==== 12 ====
next "完成。回退方式（请保存）"
{
  printf '装机时间：%s\n目标分区：%s（%s）\n新根 UUID：%s\n启动项：%s（UOS Boot UUID=%s）\n' \
    "$(date '+%F %T')" "$DEV" "$MODE_DESC" "$NEW_UUID" "$ENTRY" "$BOOT_UUID"
  printf 'custom.cfg 备份：%s\n日志：%s\n' "${CUSTOM_BAK:-（原来没有 custom.cfg）}" "$T"
} > "$T/INSTALL_RESULT.txt"
cat "$T/INSTALL_RESULT.txt"
printf '\n'
if [ "$REMOVABLE" = 1 ]; then
  cat <<EOF
  1. 回到 UOS：直接重启（或拔掉 U 盘再开机）。deepin 只在你从 UOS 里指定时才进。
  2. 以后再进 deepin：插好 U 盘，在 UOS 里 sudo grub-reboot $ENTRY，然后重启。
EOF
else
  cat <<EOF
  1. 回到 UOS：在 deepin 里 sudo hisi-next-boot uos，然后重启。
     注意：deepin 正常开机 2 分钟后会把“下次开机”设回 deepin，所以在 deepin 里直接重启还是进 deepin；
     不想要这个行为：sudo touch /etc/hisi-next-boot.off。
     deepin 开机卡住：长按电源键强制关机再开，一次性启动项已用掉，会自动回 UOS。
  2. 在 UOS 里想进 deepin：sudo grub-reboot $ENTRY，然后重启。
EOF
fi
cat <<EOF
  3. 删掉启动项：在 UOS 里编辑 /boot/grub/custom.cfg，删除 id 为 '$ENTRY' 的那段 menuentry
     （或用备份 ${CUSTOM_BAK:-——原来没有 custom.cfg，整个文件删掉即可}）。不要运行 update-grub。
  4. 释放分区：在 UOS 里 sudo mkfs.ext4 $DEV（⚠ 会清空它）$([ "$MODE" = rootb ] && printf '；deepin 的家目录在 /data/deepin25，一并删掉')。
  5. deepin 里卸载补丁：cd /opt/kirin9006c-deepin25 && sudo make uninstall；
     补丁被系统更新覆盖了：sudo hisi-patch-check --fix。
  6. 开机兜底：deepin 开机 10 分钟内登录界面没起来，会自动抓日志并重启回 UOS，
     日志在 /data/deepin25/boot-logs（没有 /data 时在 deepin 的 /var/log/d25-bootguard）；
     不想要：在 deepin 里 sudo systemctl disable d25-bootguard。
EOF
if [ "$WWP_DONE" = 1 ]; then
  printf '  7. 卸载天气壁纸：在 deepin 里 cd /opt/WeatherWallpaper/deepin-v25 && sudo ./install.sh uninstall\n'
fi
printf '\n[%s] 【总进度 100%%】装机完成，重启即可进入 deepin 25。上面的内容也保存在 %s/INSTALL_RESULT.txt\n' "$(ts)" "$T"
