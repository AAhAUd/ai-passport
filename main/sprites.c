// main/sprites.c —— 三种生物的像素精灵。见 sprites.h。
#include "sprites.h"

#include <string.h>

// ---------------------------------------------------------------- 素材
//
// 字符画：'.' 透明；字母含义见每只生物的 keys 与 colors。
// 面朝右；朝左时由 blit 做镜像。

// 喷火灯 11×5（b 身体 / h 高光 / d 描边 / e 眼睛）
static const char *const ART_TETRA[] = {
    "...hhhhh...",
    "d.hbbbbbbe.",
    "dd.bbbbbbb.",
    "d.dbbbbbb..",
    "...ddbb....",
};
static const char TETRA_KEYS[] = "bhde";
static const uint8_t TETRA_COLORS[][3] = {
    { 0xff, 0x7a, 0x1a },   // b
    { 0xff, 0xc1, 0x5a },   // h
    { 0xc4, 0x41, 0x0c },   // d
    { 0x11, 0x11, 0x11 },   // e
};

// 黄金胡子 20×7（b 身体 / h 高光 / d 描边 / e 眼睛 / w 眼白 / s 斑点）
static const char *const ART_PLECO[] = {
    "......dddd..........",
    "d....hhbbbbbbbbb.s.s",
    "dd..bbbbbbbbbbbbbe..",
    "ddd.bbbbbbbbbbbbbbw.",
    "dd..bbbbbbbbbbbbb.s.",
    "d....ddbbbbbbbb.....",
    ".....ddd.....ss.....",
};
static const char PLECO_KEYS[] = "bhdews";
static const uint8_t PLECO_COLORS[][3] = {
    { 0xd9, 0xa5, 0x2e },   // b
    { 0xf2, 0xc8, 0x5a },   // h
    { 0x8a, 0x5e, 0x12 },   // d
    { 0x11, 0x11, 0x11 },   // e
    { 0xf6, 0xe0, 0x9a },   // w 眼白
    { 0xf6, 0xe0, 0x9a },   // s 斑点
};

// 黑壳虾 9×4（l 头胸甲 / b 身体 / e 眼睛 / a 触须）
static const char *const ART_SHRIMP[] = {
    "..lll..a.",
    ".bbbbbbea",
    "bbbbbbbb.",
    "bb.b.b.b.",
};
static const char SHRIMP_KEYS[] = "lbea";
static const uint8_t SHRIMP_COLORS[][3] = {
    { 0x5a, 0x54, 0x66 },   // l
    { 0x34, 0x30, 0x3a },   // b
    { 0xe6, 0xdf, 0xf0 },   // e
    { 0x8b, 0x84, 0x96 },   // a
};

// ---------------------------------------------------------------- 内部表

#define SPR_MAX_W 20
#define SPR_MAX_H 7
#define SPR_MAX_COLORS 6

typedef struct {
    const char *const *art;
    uint8_t h;
    uint8_t w;
    const char *keys;                        // 索引 1..n 对应的字符
    const uint8_t (*colors)[3];              // 同上顺序的 RGB
} sprite_src_t;

static const sprite_src_t SOURCES[SPR_COUNT] = {
    [SPR_TETRA]  = { ART_TETRA,  5, 11, TETRA_KEYS,  TETRA_COLORS  },
    [SPR_PLECO]  = { ART_PLECO,  7, 20, PLECO_KEYS,  PLECO_COLORS  },
    [SPR_SHRIMP] = { ART_SHRIMP, 4,  9, SHRIMP_KEYS, SHRIMP_COLORS },
};

// 0 = 透明；1..n = 调色板下标
static uint8_t s_px[SPR_COUNT][SPR_MAX_H][SPR_MAX_W];
// 调色板：s_pal[kind][0] 不用，和索引对齐
static uint16_t s_pal[SPR_COUNT][SPR_MAX_COLORS + 1];
static bool s_ready;
static float s_brightness[SPR_COUNT] = { 1.0f, 1.0f, 1.0f };

// RGB565 打包并把高低字节对调（面板要求高字节先发）。
static uint16_t rgb565_be(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t v = (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
    return (uint16_t)((v >> 8) | (v << 8));
}

static void rebuild_palette(void) {
    for (int k = 0; k < SPR_COUNT; k++) {
        const sprite_src_t *src = &SOURCES[k];
        const size_t nkeys = strlen(src->keys);
        s_pal[k][0] = 0;
        for (size_t i = 0; i < nkeys && i < SPR_MAX_COLORS; i++) {
            const uint8_t *c = src->colors[i];
            const float f = s_brightness[k];
            const uint8_t r = (uint8_t)(c[0] * f > 255.0f ? 255.0f : c[0] * f);
            const uint8_t g = (uint8_t)(c[1] * f > 255.0f ? 255.0f : c[1] * f);
            const uint8_t b = (uint8_t)(c[2] * f > 255.0f ? 255.0f : c[2] * f);
            s_pal[k][i + 1] = rgb565_be(r, g, b);
        }
    }
}

// 夜间模式：喷火灯提亮一档（它是"发光"的主角），其它两种保持原样。
void sprites_set_night(bool night) {
    s_brightness[SPR_TETRA] = night ? 1.9f : 1.0f;
    rebuild_palette();
}

void sprites_init(void) {
    if (s_ready) return;
    for (int k = 0; k < SPR_COUNT; k++) {
        const sprite_src_t *src = &SOURCES[k];
        const size_t nkeys = strlen(src->keys);
        s_pal[k][0] = 0;
        for (size_t i = 0; i < nkeys && i < SPR_MAX_COLORS; i++) {
            const uint8_t *c = src->colors[i];
            s_pal[k][i + 1] = rgb565_be(c[0], c[1], c[2]);
        }
        for (int y = 0; y < src->h; y++) {
            const char *row = src->art[y];
            for (int x = 0; x < src->w; x++) {
                char ch = row[x];
                uint8_t idx = 0;
                if (ch != '.' && ch != '\0') {
                    const char *p = strchr(src->keys, ch);
                    if (p) idx = (uint8_t)(p - src->keys + 1);
                }
                s_px[k][y][x] = idx;
            }
        }
    }
    s_ready = true;
}

int sprite_w(sprite_kind_t kind) {
    return (kind >= 0 && kind < SPR_COUNT) ? SOURCES[kind].w : 0;
}

int sprite_h(sprite_kind_t kind) {
    return (kind >= 0 && kind < SPR_COUNT) ? SOURCES[kind].h : 0;
}

void sprite_blit(uint16_t *dst, int dst_w, int dst_h,
                 sprite_kind_t kind, int x, int y, bool mirror) {
    if (!dst || kind < 0 || kind >= SPR_COUNT) return;
    sprites_init();
    const sprite_src_t *src = &SOURCES[kind];
    for (int sy = 0; sy < src->h; sy++) {
        const int dy = y + sy;
        if (dy < 0 || dy >= dst_h) continue;
        for (int sx = 0; sx < src->w; sx++) {
            const int dx = x + sx;
            if (dx < 0 || dx >= dst_w) continue;
            const int sxx = mirror ? (src->w - 1 - sx) : sx;
            const uint8_t v = s_px[kind][sy][sxx];
            if (v == 0) continue;
            dst[(size_t)dy * dst_w + dx] = s_pal[kind][v];
        }
    }
}
