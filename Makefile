# 在目标机器（deepin 25 arm64，Rootb）上编译和安装。
#   依赖：gcc make pkg-config libwayland-dev libdrm-dev libegl-dev libglvnd-core-dev
#   顺序：sudo scripts/fetch-uos-blobs.sh <UOS 根分区>   # 先放好厂商 libmali（libmali-compat 链接时要用）
#         make
#         sudo make install
#   演练：make install DESTDIR=/tmp/stage
CC      ?= gcc
CFLAGS  ?= -O2 -Wall
DESTDIR ?=
MALI_DIR = /usr/local/lib/hisi-mali
B        = build

LIBS = $(B)/libEGL_hisi_mali.so.0 $(B)/libmali-compat.so $(B)/libtl-malibuf.so \
       $(B)/libtl-norender.so $(B)/libnm-station-fix.so

all: $(LIBS)

$(B):
	mkdir -p $@

$(B)/libEGL_hisi_mali.so.0: src/hisi-mali-egl.c | $(B)
	$(CC) $(CFLAGS) -shared -fPIC -fvisibility=hidden -o $@ $< \
		-Wl,-soname,libEGL_hisi_mali.so.0 -lwayland-client -ldl -lpthread

$(B)/libmali-compat.so: src/mali-compat.c | $(B)
	@test -e $(MALI_DIR)/libmali.so.0 || { echo "缺少 $(MALI_DIR)/libmali.so.0：先运行 sudo scripts/fetch-uos-blobs.sh"; exit 1; }
	$(CC) $(CFLAGS) -shared -fPIC -o $@ $< -L$(MALI_DIR) -l:libmali.so.0 -lwayland-client -ldl

$(B)/libtl-malibuf.so: src/tl-malibuf.c | $(B)
	$(CC) $(CFLAGS) -shared -fPIC -o $@ $< $$(pkg-config --cflags --libs libdrm wayland-server) -ldl

$(B)/libtl-norender.so: src/tl-norender.c | $(B)
	$(CC) $(CFLAGS) -shared -fPIC -o $@ $< -ldl

$(B)/libnm-station-fix.so: src/nm-station-fix.c | $(B)
	$(CC) $(CFLAGS) -shared -fPIC -o $@ $< -ldl

# 正在被 treeland / NetworkManager 映射的 .so 不能原地覆盖：先装成临时名，再 rename 替换
define put_so
	install -m 755 $(1) $(DESTDIR)$(2)/.$(notdir $(1)).new
	mv -f $(DESTDIR)$(2)/.$(notdir $(1)).new $(DESTDIR)$(2)/$(notdir $(1))
endef

install: all
	install -d $(DESTDIR)$(MALI_DIR) $(DESTDIR)/usr/local/lib/hisi-treeland $(DESTDIR)/usr/local/lib/hisi-nmfix
	$(call put_so,$(B)/libEGL_hisi_mali.so.0,$(MALI_DIR))
	$(call put_so,$(B)/libmali-compat.so,$(MALI_DIR))
	$(call put_so,$(B)/libtl-malibuf.so,$(MALI_DIR))
	$(call put_so,$(B)/libtl-norender.so,/usr/local/lib/hisi-treeland)
	$(call put_so,$(B)/libnm-station-fix.so,/usr/local/lib/hisi-nmfix)
	install -m 644 src/hisi-mali-egl.c src/mali-compat.c src/tl-malibuf.c $(DESTDIR)$(MALI_DIR)/
	install -m 644 src/tl-norender.c $(DESTDIR)/usr/local/lib/hisi-treeland/
	install -m 644 src/nm-station-fix.c $(DESTDIR)/usr/local/lib/hisi-nmfix/
	cd rootfs && find . -type f ! -path './usr/local/sbin/*' ! -name apt-pre-hook ! -name egl-deny \
		-exec install -D -m 644 {} $(DESTDIR)/{} \;
	cd rootfs && find ./usr/local/sbin -type f -exec install -D -m 755 {} $(DESTDIR)/{} \;
	install -D -m 755 rootfs/usr/local/lib/hisi-patch/apt-pre-hook $(DESTDIR)/usr/local/lib/hisi-patch/apt-pre-hook
	test -e $(DESTDIR)/etc/hisi-mali/egl-deny || install -D -m 644 rootfs/etc/hisi-mali/egl-deny $(DESTDIR)/etc/hisi-mali/egl-deny
	@echo
	@echo "已安装。接下来（见 README“安装”一节）："
	@echo "  sudo systemctl daemon-reload"
	@echo "  sudo systemctl enable --now hisi-ddm-watchdog.service hisi-patch-check.timer"
	@echo "  sudo systemctl enable hisi-patch-check.service"
	@echo "  sudo systemctl restart NetworkManager wpa_supplicant treeland   # treeland 重启会回到锁屏"
	@echo "  sudo hisi-patch-check snapshot"

clean:
	rm -rf $(B)

.PHONY: all install clean
