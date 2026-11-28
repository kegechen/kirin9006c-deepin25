// gpu-conv：在 Mali GPU 上把采集到的 XRGB 画面缩放并转成 I420（BT.709 有限范围）
#pragma once
#include <stdint.h>

// drm_fd：已通过 DRM 认证的 card0；src_*：screencopy 用的 dumb buffer（PRIME fd、fourcc、尺寸、行距）。
// 要求 out_w 是 8 的倍数、out_h 是偶数。成功返回 0；失败打印原因并返回 -1，调用方退回 CPU 路径。
int gpu_init(int drm_fd, int src_fd, uint32_t src_fourcc, int src_w, int src_h, uint32_t src_pitch,
             int out_w, int out_h);
// 对源缓冲区当前内容做一次转换，返回时 GPU 已完成，源缓冲区可以交给合成器写下一帧
void gpu_convert(void);
// 把转换结果读到三个平面（CPU 读写合并内存）
void gpu_readout(uint8_t *y, int ys, uint8_t *u, int us, uint8_t *v, int vs);
