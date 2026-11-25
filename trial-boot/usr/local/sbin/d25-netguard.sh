#!/bin/bash
# deepin25 试启动兜底（multi-user 阶段版）：10 分钟内网络不通就抓日志并重启回 UOS。只在试启动阶段 enable。
# PING_HOST 默认取当时的默认网关，也可以在 service 里用 Environment=PING_HOST=... 指定。
D=/var/log/d25-boot; mkdir -p $D; B=$(date +%Y%m%d-%H%M%S)
dump() { journalctl -b --no-pager > $D/$B-journal.txt 2>&1; dmesg > $D/$B-dmesg.txt 2>&1
         { ip a; nmcli dev; nmcli con show --active; systemctl --failed --no-pager; } > $D/$B-state.txt 2>&1; sync; }
target() { [ -n "${PING_HOST:-}" ] && echo "$PING_HOST" || ip route show default 2>/dev/null | awk '{print $3; exit}'; }
for i in $(seq 1 60); do
  h=$(target)
  if [ -n "$h" ] && ping -c1 -W2 "$h" >/dev/null 2>&1; then
    echo "network ok after ~$((i*10))s" > $D/$B-ok.txt; sleep 120; dump; exit 0
  fi
  sleep 10
done
echo "no network in 10min -> reboot" > $D/$B-reboot.txt; dump; systemctl reboot
