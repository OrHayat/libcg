#ifndef COLOR_H
#define COLOR_H

#include "util/common.h"

/* Pixel format everywhere is 0xAARRGGBB. Framebuffer contents are
   premultiplied; colors typed by the user (hex input) are straight
   alpha until color_premultiply is applied. */

static inline int color_hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parses RGB, RRGGBB or RRGGBBAA (no leading '#'). Result is straight
   (unpremultiplied) ARGB; alpha defaults to 0xFF when absent. */
static inline bool color_parse_hex(const char *s, int len, u32 *out) {
    for (int i = 0; i < len; i++) {
        if (color_hex_digit(s[i]) < 0) return false;
    }
    if (len == 3) {
        u8 r = (u8)color_hex_digit(s[0]); r = (u8)((r << 4) | r);
        u8 g = (u8)color_hex_digit(s[1]); g = (u8)((g << 4) | g);
        u8 b = (u8)color_hex_digit(s[2]); b = (u8)((b << 4) | b);
        *out = RGB(r, g, b);
        return true;
    } else if (len == 6 || len == 8) {
        u8 r = (u8)((color_hex_digit(s[0]) << 4) | color_hex_digit(s[1]));
        u8 g = (u8)((color_hex_digit(s[2]) << 4) | color_hex_digit(s[3]));
        u8 b = (u8)((color_hex_digit(s[4]) << 4) | color_hex_digit(s[5]));
        u8 a = len == 8 ? (u8)((color_hex_digit(s[6]) << 4) | color_hex_digit(s[7])) : 0xFF;
        *out = RGBA(r, g, b, a);
        return true;
    }
    return false;
}

static inline u32 color_premultiply(u32 argb) {
    u8 a = (u8)((argb >> 24) & 0xFF);
    u8 r = (u8)((argb >> 16) & 0xFF);
    u8 g = (u8)((argb >>  8) & 0xFF);
    u8 b = (u8)( argb        & 0xFF);
    r = (u8)((r * a) / 255);
    g = (u8)((g * a) / 255);
    b = (u8)((b * a) / 255);
    return RGBA(r, g, b, a);
}

/* Source-over. Both operands premultiplied; result = src + dst * (1 - src_a). */
static inline u32 color_blend(u32 dst, u32 src) {
    u8 sa = (u8)((src >> 24) & 0xFF);
    u8 sr = (u8)((src >> 16) & 0xFF);
    u8 sg = (u8)((src >>  8) & 0xFF);
    u8 sb = (u8)( src        & 0xFF);
    u8 da = (u8)((dst >> 24) & 0xFF);
    u8 dr = (u8)((dst >> 16) & 0xFF);
    u8 dg = (u8)((dst >>  8) & 0xFF);
    u8 db = (u8)( dst        & 0xFF);
    u8 inv = (u8)(255 - sa);
    u8 r = (u8)(sr + (dr * inv) / 255);
    u8 g = (u8)(sg + (dg * inv) / 255);
    u8 b = (u8)(sb + (db * inv) / 255);
    u8 a = (u8)(sa + (da * inv) / 255);
    return RGBA(r, g, b, a);
}

#endif /* COLOR_H */
