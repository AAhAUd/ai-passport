// main/sprites.h —— 三种生物的像素精灵与通用 blit。
//
// 纯计算：不碰 ESP-IDF、不碰显示驱动，可以直接在主机上测（写进普通 uint16 缓冲）。
//
// 精灵素材用字符画描述（'.' 透明，其余字母查色表），启动时转成索引表 ——
// 源码里能直接看出形状，改图不用数数组下标。
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    SPR_TETRA = 0,     // 喷火灯 11×5
    SPR_PLECO,         // 黄金胡子 20×7
    SPR_SHRIMP,        // 黑壳虾 9×4
    SPR_COUNT
} sprite_kind_t;

// 建立索引表与调色板（已换字节序）。重复调用安全。
void sprites_init(void);

int sprite_w(sprite_kind_t kind);
int sprite_h(sprite_kind_t kind);

// 把精灵画进一块 dst_w × dst_h 的 RGB565 缓冲，左上角在 (x, y)（可为负）。
// mirror = true 时水平镜像。超出缓冲的部分裁掉；透明像素不写。
void sprite_blit(uint16_t *dst, int dst_w, int dst_h,
                 sprite_kind_t kind, int x, int y, bool mirror);
