# 在 L540 上构建：make
# 依赖：gcc、wayland-scanner、libwayland-dev、libdrm-dev、libx264（运行库已随 ffmpeg 装好）。
# 没有 libx264-dev 时自动 apt-get download 到 deps/ 只取头文件，不需要 sudo。

CC      ?= gcc
CFLAGS  ?= -O3 -Wall -Wno-unused-result
PROTOS  := wlr-screencopy-unstable-v1 linux-dmabuf-unstable-v1
PROTO_H := $(PROTOS:%=%-client-protocol.h)
PROTO_C := $(PROTOS:%=%-protocol.c)

X264_SO  := $(firstword $(wildcard /usr/lib/*/libx264.so.[0-9]*))
X264_HDR := $(if $(wildcard /usr/include/x264.h),,deps/root/usr/include/x264.h)
CPPFLAGS += -I. -I/usr/include/libdrm $(if $(X264_HDR),-Ideps/root/usr/include)

all: rv-cap scbench

# libmali（gpu-conv.c 运行时 dlopen）要从主程序解析这三个符号，见 gpu-conv.c
MALI_SYMS := wl_proxy_ref wl_proxy_unref wl_proxy_get_refcount

rv-cap: rv-cap.c gpu-conv.c gpu-conv.h $(PROTO_C) $(PROTO_H) $(X264_HDR)
	@test -n "$(X264_SO)" || { echo "找不到 libx264 运行库"; exit 1; }
	$(CC) $(CFLAGS) -fopenmp -pthread $(CPPFLAGS) -o $@ rv-cap.c gpu-conv.c $(PROTO_C) \
		$(MALI_SYMS:%=-Wl,--export-dynamic-symbol=%) \
		-lwayland-client -ldrm -l:$(notdir $(X264_SO)) -lm -ldl

scbench: scbench.c $(PROTO_C) $(PROTO_H)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ scbench.c $(PROTO_C) -lwayland-client -ldrm

%-client-protocol.h: protocols/%.xml
	wayland-scanner client-header $< $@

%-protocol.c: protocols/%.xml
	wayland-scanner private-code $< $@

deps/root/usr/include/x264.h:
	mkdir -p deps
	cd deps && apt-get download libx264-dev && dpkg -x libx264-dev_*.deb root

clean:
	rm -f rv-cap scbench $(PROTO_H) $(PROTO_C)

.PHONY: all clean
