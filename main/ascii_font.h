// main/ascii_font.h —— ASCII 8x8 点阵字模。
//
// 只覆盖可打印 ASCII（0x20..0x7E）。字形来自 font8x8_basic
// (Daniel Hepper, Public Domain)，取整段比逐个挑字更省事，整表仅 760 字节。
#pragma once

#include <stdint.h>

#define ASCII_FONT_FIRST 0x20
#define ASCII_FONT_LAST  0x7E
#define ASCII_FONT_COUNT (ASCII_FONT_LAST - ASCII_FONT_FIRST + 1)

// 每字形 8 行，行内最高位是最左像素。
extern const uint8_t ascii_font8x8[ASCII_FONT_COUNT][8];

// 取字形行数据；不可打印字符返回空格的字形。
const uint8_t *ascii_font_glyph(char c);
