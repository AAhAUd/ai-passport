// tests/test_sprites.c —— 精灵 blit 的主机测试。
//
// blit 是纯函数（写普通 uint16 缓冲），所以镜像、透明、裁剪都能在主机上验，
// 不用烧板子。
#include <assert.h>
#include <string.h>

#include "sprites.h"

#define DW 40
#define DH 20

static uint16_t buf[DH][DW];

static void clear_buf(void) { memset(buf, 0, sizeof(buf)); }

static int count_nonzero(void) {
    int n = 0;
    for (int y = 0; y < DH; y++)
        for (int x = 0; x < DW; x++)
            if (buf[y][x]) n++;
    return n;
}

static void test_sizes(void) {
    sprites_init();
    assert(sprite_w(SPR_TETRA) == 11 && sprite_h(SPR_TETRA) == 5);
    assert(sprite_w(SPR_PLECO) == 20 && sprite_h(SPR_PLECO) == 7);
    assert(sprite_w(SPR_SHRIMP) == 9 && sprite_h(SPR_SHRIMP) == 4);
}

static void test_blit_writes_some_pixels(void) {
    clear_buf();
    sprite_blit(&buf[0][0], DW, DH, SPR_TETRA, 5, 5, false);
    const int n = count_nonzero();
    assert(n > 20);           // 喷火灯有几十个不透明像素
    assert(n < 11 * 5);       // 但不是整块矩形（有透明像素）
}

static void test_mirror_is_horizontal_flip(void) {
    static uint16_t a[DH][DW], b[DH][DW];
    clear_buf();
    sprite_blit(&buf[0][0], DW, DH, SPR_TETRA, 4, 4, false);
    memcpy(a, buf, sizeof(buf));
    clear_buf();
    sprite_blit(&buf[0][0], DW, DH, SPR_TETRA, 4, 4, true);
    memcpy(b, buf, sizeof(buf));

    const int w = sprite_w(SPR_TETRA);
    for (int y = 0; y < DH; y++)
        for (int x = 0; x < DW; x++) {
            int mx = (x >= 4 && x < 4 + w) ? (4 + w - 1 - (x - 4)) : x;
            assert(a[y][x] == b[y][mx]);
        }
}

static void test_clipping_does_not_write_out_of_bounds(void) {
    clear_buf();
    // 左上角部分越界
    sprite_blit(&buf[0][0], DW, DH, SPR_PLECO, -5, -3, false);
    // 右下角部分越界
    sprite_blit(&buf[0][0], DW, DH, SPR_PLECO, DW - 5, DH - 3, false);
    // 完全在界外：不应写任何像素
    clear_buf();
    sprite_blit(&buf[0][0], DW, DH, SPR_TETRA, -50, -50, false);
    sprite_blit(&buf[0][0], DW, DH, SPR_TETRA, DW + 50, DH + 50, false);
    assert(count_nonzero() == 0);
}

static void test_bad_arguments_are_ignored(void) {
    clear_buf();
    sprite_blit(&buf[0][0], DW, DH, SPR_COUNT, 0, 0, false);
    assert(count_nonzero() == 0);
    sprite_blit(NULL, DW, DH, SPR_TETRA, 0, 0, false);   // 不应崩溃
}

int main(void) {
    test_sizes();
    test_blit_writes_some_pixels();
    test_mirror_is_horizontal_flip();
    test_clipping_does_not_write_out_of_bounds();
    test_bad_arguments_are_ignored();
    return 0;
}
