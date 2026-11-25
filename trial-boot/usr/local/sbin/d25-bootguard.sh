#!/bin/bash
# deepin25 试启动兜底（只在最初几次试启动时 enable，稳定后 disable）。
# 开机 3 分钟时抓一份状态；之后 10 分钟内一直 ping 不通 PING_HOST 就用 sysrq 重启
# （grub-reboot 的一次性启动项已被消耗，重启会回到 UOS）。
# 诊断一律放后台、不等待（内核里卡 D 状态的命令 kill 不掉），重启用 sysrq b。
# PING_HOST 默认取当时的默认网关，也可以在 service 里用 Environment=PING_HOST=... 指定。
L=/var/log/d25-boot
B=$(date +%Y%m%d-%H%M%S)
dump() {
  local tag=$1 p=$L/$B-$tag
  mount -o remount,rw / 2>/dev/null; mkdir -p $L
  ( { echo "== $tag $(date) uptime=$(cut -d' ' -f1 /proc/uptime)"
      for n in /sys/class/net/*; do echo "${n##*/} $(cat $n/operstate 2>/dev/null)"; done
      ps -eo pid,stat,wchan:32,etime,cmd --sort=pid | awk '$2 ~ /D/ || NR==1'
      echo "-- all"; ps -eo pid,stat,wchan:32,etime,cmd --sort=pid | head -200
    } > $p-state.txt 2>&1
    timeout 20 systemctl list-jobs --no-pager > $p-jobs.txt 2>&1
    timeout 20 systemctl --failed --no-pager >> $p-jobs.txt 2>&1
    dmesg > $p-dmesg.txt 2>&1
    timeout 30 journalctl -b --no-pager -o short-monotonic > $p-journal.txt 2>&1
    sync ) &
}
target() { [ -n "${PING_HOST:-}" ] && echo "$PING_HOST" || ip route show default 2>/dev/null | awk '{print $3; exit}'; }
sleep 180; dump t3m
for i in $(seq 1 50); do
  h=$(target)
  if [ -n "$h" ] && timeout 5 ping -c1 -W2 "$h" >/dev/null 2>&1; then sleep 60; dump ok; exit 0; fi
  sleep 6
done
dump reboot
sleep 40
sync &
sleep 5
echo b > /proc/sysrq-trigger
