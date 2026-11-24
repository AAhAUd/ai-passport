// main/aquarium_bg.h —— 像素底图的契约。数据由 tools/gen_background.py 生成。
//
// index 0 是"水"：底图里不存水体颜色，渲染时按行取运行时算好的渐变值。
// 其余索引指向 s_bg_pal，颜色已做高低字节对调（面板要求高字节先发）。
#pragma once

#include <stdint.h>

#define BG_W 240
#define BG_H 320
#define BG_WATER_INDEX 0

// 调色板长度由生成物给出（夜间调色板要按数量派生）。
extern const unsigned s_bg_pal_count;
extern const uint16_t s_bg_pal[];

// 逐行存放，行内从左到右；长度 BG_H * BG_W。
extern const uint8_t s_bg_idx[BG_H * BG_W];
