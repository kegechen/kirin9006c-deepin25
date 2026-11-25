#!/bin/sh
# 安装 compat v4 + 客户端 libmali 启用配置（以 root 运行；源文件在 /tmp/mc4）
set -e
M=/usr/local/lib/hisi-mali
S=/tmp/mc4

# 1) compat v4：先备份 v3，再用 rename 原子替换（treeland 正映射着旧文件，不能原地覆盖）
[ -e /root/libmali-compat.so.v3 ] || cp -a $M/libmali-compat.so /root/libmali-compat.so.v3
[ -e /root/mali-compat.c.v3 ] || cp -a $M/mali-compat.c /root/mali-compat.c.v3
install -m 755 $S/libmali-compat.so $M/.libmali-compat.so.new
mv -f $M/.libmali-compat.so.new $M/libmali-compat.so
install -m 644 $S/mali-compat.c $M/mali-compat.c

# 2) 通用包装脚本 + PATH 优先的符号链接（treeland 按名字拉起壁纸进程；mpv 从终端/桌面文件按名字启动）
install -m 755 $S/mali-exec $M/mali-exec
for n in treeland-wallpaper-factory mpv; do
    ln -sfn $M/mali-exec /usr/local/bin/$n
done

# 3) 用户服务 drop-in：dde-shell（任务栏/桌面）、控制中心（D-Bus 激活与应用管理器两条启动路径）
for u in 'dde-shell@.service' 'dde-shell-plugin@.service' 'org.deepin.dde.control-center.service' \
         'app-DDE-org.deepin.dde.control\x2dcenter@.service'; do
    mkdir -p "/etc/systemd/user/$u.d"
    install -m 644 $S/hisi-mali-user.conf "/etc/systemd/user/$u.d/hisi-mali.conf"
done
ls -la $M /usr/local/bin
find /etc/systemd/user -name hisi-mali.conf
