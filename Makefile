# 在目标机器（deepin 25 arm64，Rootb）上编译和安装。
#   依赖：gcc g++ make pkg-config libwayland-dev libdrm-dev libegl-dev libglvnd-core-dev libglib2.0-dev libssl-dev
#         qt6-base-dev qt6-base-dev-tools qt6-declarative-dev（libtl-lockbio：moc、rcc、Qt6Core/DBus/Qml）
#   顺序：sudo scripts/fetch-uos-blobs.sh <UOS 根分区>   # 先放好厂商 libmali（libmali-compat 链接时要用）
#         make
#         sudo make install
#   演练：make install DESTDIR=/tmp/stage
CC      ?= gcc
CFLAGS  ?= -O2 -Wall
CXX     ?= g++
CXXFLAGS ?= -O2 -Wall
QT_LIBEXEC ?= /usr/lib/qt6/libexec
DESTDIR ?=
MALI_DIR = /usr/local/lib/hisi-mali
B        = build

LIBS = $(B)/libEGL_hisi_mali.so.0 $(B)/libmali-compat.so $(B)/libtl-malibuf.so \
       $(B)/libtl-norender.so $(B)/libnm-station-fix.so $(B)/libtl-lockbio.so
BINS = $(B)/hisi-bio-unlock $(B)/hisi-phone-link
BIO_SRC = src/hisi-bio-unlock.c src/hisi-bio-phone.c src/hisi-bio-phonectl.c

all: $(LIBS) $(BINS)

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

# 锁屏指纹 / 人脸 + 手机解锁的验签、配对（root）；手机的 Wi-Fi / 蓝牙收发另由低权限的 hisi-phone-link 做
$(B)/hisi-bio-unlock: $(BIO_SRC) src/hisi-bio.h | $(B)
	$(CC) $(CFLAGS) -o $@ $(BIO_SRC) $$(pkg-config --cflags --libs gio-2.0 gio-unix-2.0 libcrypto)

$(B)/hisi-phone-link: src/hisi-phone-link.c | $(B)
	$(CC) $(CFLAGS) -o $@ $< $$(pkg-config --cflags --libs gio-2.0 gio-unix-2.0)

# 锁屏界面的指纹 / 人脸入口：QML 经 rcc --binary 编好后 .incbin 进 .so
$(B)/lockbio.rcc: src/lockbio/lockbio.qrc src/lockbio/qmldir src/lockbio/UserInputHisi.qml | $(B)
	$(QT_LIBEXEC)/rcc --binary -o $@ $<

$(B)/tl-lockbio.moc: src/tl-lockbio.cpp | $(B)
	$(QT_LIBEXEC)/moc -o $@ $<

$(B)/libtl-lockbio.so: src/tl-lockbio.cpp $(B)/tl-lockbio.moc $(B)/lockbio.rcc
	$(CXX) $(CXXFLAGS) -std=c++17 -shared -fPIC -fvisibility=hidden -o $@ $< -I$(B) \
		-DHISI_LOCKBIO_RCC='"$(abspath $(B)/lockbio.rcc)"' $$(pkg-config --cflags --libs Qt6Core Qt6DBus Qt6Qml)

# 正在被 treeland / NetworkManager 映射的 .so 不能原地覆盖：先装成临时名，再 rename 替换
define put_so
	install -m 755 $(1) $(DESTDIR)$(2)/.$(notdir $(1)).new
	mv -f $(DESTDIR)$(2)/.$(notdir $(1)).new $(DESTDIR)$(2)/$(notdir $(1))
endef

install: all
	install -d $(DESTDIR)$(MALI_DIR) $(DESTDIR)/usr/local/lib/hisi-treeland $(DESTDIR)/usr/local/lib/hisi-nmfix \
		$(DESTDIR)/usr/local/lib/hisi-bio
	$(call put_so,$(B)/libEGL_hisi_mali.so.0,$(MALI_DIR))
	$(call put_so,$(B)/libmali-compat.so,$(MALI_DIR))
	$(call put_so,$(B)/libtl-malibuf.so,$(MALI_DIR))
	$(call put_so,$(B)/libtl-norender.so,/usr/local/lib/hisi-treeland)
	$(call put_so,$(B)/libtl-lockbio.so,/usr/local/lib/hisi-treeland)
	$(call put_so,$(B)/libnm-station-fix.so,/usr/local/lib/hisi-nmfix)
	install -m 644 src/hisi-mali-egl.c src/mali-compat.c src/tl-malibuf.c $(DESTDIR)$(MALI_DIR)/
	install -m 644 src/tl-norender.c src/tl-lockbio.cpp $(DESTDIR)/usr/local/lib/hisi-treeland/
	install -m 644 src/nm-station-fix.c $(DESTDIR)/usr/local/lib/hisi-nmfix/
	$(call put_so,$(B)/hisi-bio-unlock,/usr/local/lib/hisi-bio)
	$(call put_so,$(B)/hisi-phone-link,/usr/local/lib/hisi-bio)
	install -m 644 $(BIO_SRC) src/hisi-bio.h src/hisi-phone-link.c $(DESTDIR)/usr/local/lib/hisi-bio/
	cd rootfs && find . -type f ! -path './usr/local/sbin/*' ! -name apt-pre-hook ! -name wait-dpkg-idle ! -name egl-deny \
		-exec install -D -m 644 {} $(DESTDIR)/{} \;
	cd rootfs && find ./usr/local/sbin -type f -exec install -D -m 755 {} $(DESTDIR)/{} \;
	install -D -m 755 rootfs/usr/local/lib/hisi-patch/apt-pre-hook $(DESTDIR)/usr/local/lib/hisi-patch/apt-pre-hook
	install -D -m 755 rootfs/usr/local/lib/hisi-patch/wait-dpkg-idle $(DESTDIR)/usr/local/lib/hisi-patch/wait-dpkg-idle
	test -e $(DESTDIR)/etc/hisi-mali/egl-deny || install -D -m 644 rootfs/etc/hisi-mali/egl-deny $(DESTDIR)/etc/hisi-mali/egl-deny
	@[ -n "$(NOHINT)" ] || { echo; \
	  echo "已安装。接下来（见 README“安装”一节）："; \
	  echo "  sudo systemctl daemon-reload"; \
	  echo "  sudo systemctl enable --now hisi-ddm-watchdog.service hisi-patch-check.timer"; \
	  echo "  sudo systemctl enable hisi-patch-check.service"; \
	  echo "  sudo systemctl enable --now hisi-strip-selinux-xattr.service   # 在 UOS 下装的系统才需要，玲珑应用要靠它"; \
	  echo "  sudo systemctl reload dbus                            # 让 org.hisi.BioUnlock1 的策略生效（锁屏指纹 / 人脸图标）"; \
	  echo "  sudo systemctl enable --now hisi-bio-unlock.service   # 锁屏指纹 / 人脸（已在运行就 restart）"; \
		  echo "  sudo systemd-sysusers && sudo systemctl reload dbus   # 建 hisi-phone 用户并让它的 D-Bus 规则生效"; \
		  echo "  sudo systemctl enable --now hisi-phone-link.service   # 手机指纹解锁（Wi-Fi / 蓝牙）；配对：sudo hisi-phone pair"; \
	  echo "  sudo hisi-dcc-bio-entry                               # 控制中心放出“生物认证”录入入口"; \
	  echo "  sudo systemctl restart NetworkManager wpa_supplicant treeland   # treeland 重启会回到锁屏"; \
	  echo "  sudo hisi-patch-check snapshot"; }

clean:
	rm -rf $(B)

# 只删本仓库装的文件，不动厂商 blob、用户信息、壁纸补丁和已生成的基线
uninstall:
	rm -f $(DESTDIR)$(MALI_DIR)/libEGL_hisi_mali.so.0 $(DESTDIR)$(MALI_DIR)/libmali-compat.so \
		$(DESTDIR)$(MALI_DIR)/libtl-malibuf.so $(DESTDIR)/usr/local/lib/hisi-treeland/libtl-norender.so \
		$(DESTDIR)/usr/local/lib/hisi-treeland/libtl-lockbio.so \
		$(DESTDIR)/usr/local/lib/hisi-nmfix/libnm-station-fix.so $(DESTDIR)/usr/local/lib/hisi-bio/hisi-bio-unlock \
		$(DESTDIR)/usr/local/lib/hisi-bio/hisi-phone-link
	cd rootfs && find . -type f ! -path './usr/local/sbin/*' ! -name apt-pre-hook ! -name wait-dpkg-idle \
		-exec rm -f $(DESTDIR)/{} \;
	cd rootfs && find ./usr/local/sbin -type f -exec rm -f $(DESTDIR)/{} \;
	@echo "已移除补丁文件；/usr/local/lib/hisi-mali 里的厂商 blob 和基线未动，确认后再重启服务。"

.PHONY: all install clean uninstall
