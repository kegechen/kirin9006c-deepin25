# 试启动兜底

最初几次从 UOS `grub-reboot` 进 deepin 时用。那时 deepin 可能卡在开机过程中，没有屏幕输出，也连不上 SSH。这些脚本负责抓现场日志，并在起不来的时候自动重启回 UOS（一次性启动项已被消耗，重启会回到默认的 UOS）。系统稳定后全部 disable。

| 文件 | 作用 |
|---|---|
| `usr/local/sbin/d25-init-probe` | 早期的 init 包装：后台留一个探针，75 秒后抓 PID 1 及所有进程的内核栈和系统调用，写到 `/var/log/d25-boot/`，然后 sysrq 重启。elfverify 卡住 systemd 就是靠它抓到的。现在 `d25-init` 里带了同样的探针（内核参数 `d25.probe=1`，只在 PID 1 卡在 clone 时才触发） |
| `usr/local/sbin/d25-bootguard.sh` + `.service` | sysinit 阶段启动：3 分钟时抓一份状态，之后约 10 分钟内一直 ping 不通就抓日志并 sysrq 重启 |
| `usr/local/sbin/d25-netguard.sh` + `.service` | multi-user 阶段版：10 分钟内网络不通就抓日志并 `systemctl reboot` |

两个 guard 默认 ping 当时的默认网关，也可以在 service 里加 `Environment=PING_HOST=<地址>` 指定。日志在 `/var/log/d25-boot/`，回到 UOS 后挂上 Rootb 就能看。

```sh
sudo install -m 755 usr/local/sbin/* /usr/local/sbin/
sudo install -m 644 etc/systemd/system/*.service /etc/systemd/system/
sudo systemctl enable d25-bootguard.service      # 或 d25-netguard.service
# 稳定后：
sudo systemctl disable d25-bootguard.service d25-netguard.service
```

`hisi-patch-check` 会检查这两个服务处于 disabled 状态。
