#ifndef COLOR_H
#define COLOR_H

#include "util/common.h"

/* ============================================================
   Pixel format is 0xAARRGGBB everywhere. Two distinct types carry the
   one distinction a u32 cannot:

     color_t    straight (unpremultiplied) alpha — what the user types
                at the '#' prompt, what an image file holds
     pcolor_t   premultiplied — what the framebuffer and the paint canvas
                hold, and the only thing blending accepts

   Same layout, separate types, so handing a straight colour to something
   expecting premultiplied is a compile error instead of a picture with
   the background punched out.

   Channels are declared b,g,r,a because the target is little-endian: in
   0xAARRGGBB the blue byte sits at the lowest address. A big-endian port
   reverses that order.

   `.rgb` names the low 24 bits as one assignable value, so colour can be
   read or written without disturbing alpha. Bit-field allocation order is
   implementation-defined, but clang, GCC and MSVC all fill from the low
   bit on little-endian, which covers macOS, Win32 and X11.
   ============================================================ */

#define COLOR_MEMBERS_                                 \
    u32 rgba;                     /* packed 0xAARRGGBB */   \
    struct { u8 b, g, r, a; };    /* per channel       */   \
    struct { u32 rgb : 24,        /* colour, no alpha  */   \
                 alpha : 8; };                              \
    u8 ch[4]                      /* indexable         */

typedef union { COLOR_MEMBERS_; } color_t;    /* straight       */
typedef union { COLOR_MEMBERS_; } pcolor_t;   /* premultiplied  */

/* Constructors. An opaque colour is identical in both representations,
   so PCOLOR_RGB needs no conversion step. */
#define COLOR_RGB(r, g, b)      ((color_t){  .rgba = RGB(r, g, b) })
#define COLOR_RGBA(r, g, b, a)  ((color_t){  .rgba = RGBA(r, g, b, a) })
#define PCOLOR_RGB(r, g, b)     ((pcolor_t){ .rgba = RGB(r, g, b) })
#define PCOLOR_CLEAR            ((pcolor_t){ .rgba = 0 })          /* fully transparent */

/* The framebuffer is a plain u32 buffer: platform.h deliberately knows
   nothing about render types, so phase 9's backends stay unaffected.
   Viewing that buffer as pcolor_t is well-defined rather than a pun — C
   permits access to an object through a union type that lists the object's
   own type among its members. */
static inline pcolor_t *pcolor_pixels(u32 *raw) { return (pcolor_t *)raw; }

static inline int color_hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parses RGB, RRGGBB or RRGGBBAA (no leading '#'). Alpha defaults to 0xFF. */
static inline bool color_parse_hex(const char *s, int len, color_t *out) {
    for (int i = 0; i < len; i++)
        if (color_hex_digit(s[i]) < 0) return false;

    if (len == 3) {
        u8 r = (u8)color_hex_digit(s[0]); r = (u8)((r << 4) | r);
        u8 g = (u8)color_hex_digit(s[1]); g = (u8)((g << 4) | g);
        u8 b = (u8)color_hex_digit(s[2]); b = (u8)((b << 4) | b);
        *out = COLOR_RGB(r, g, b);
        return true;
    }
    if (len == 6 || len == 8) {
        u8 r = (u8)((color_hex_digit(s[0]) << 4) | color_hex_digit(s[1]));
        u8 g = (u8)((color_hex_digit(s[2]) << 4) | color_hex_digit(s[3]));
        u8 b = (u8)((color_hex_digit(s[4]) << 4) | color_hex_digit(s[5]));
        u8 a = len == 8 ? (u8)((color_hex_digit(s[6]) << 4) | color_hex_digit(s[7])) : 0xFF;
        *out = COLOR_RGBA(r, g, b, a);
        return true;
    }
    return false;
}

/* The one place straight becomes premultiplied. */
static inline pcolor_t color_premultiply(color_t c) {
    pcolor_t p;
    p.a = c.a;
    p.r = (u8)((c.r * c.a) / 255);
    p.g = (u8)((c.g * c.a) / 255);
    p.b = (u8)((c.b * c.a) / 255);
    return p;
}

/* Source-over: result = src + dst * (1 - src.a). Both premultiplied. */
static inline pcolor_t color_blend(pcolor_t dst, pcolor_t src) {
    u8 inv = (u8)(255 - src.a);
    pcolor_t o;
    o.r = (u8)(src.r + (dst.r * inv) / 255);
    o.g = (u8)(src.g + (dst.g * inv) / 255);
    o.b = (u8)(src.b + (dst.b * inv) / 255);
    o.a = (u8)(src.a + (dst.a * inv) / 255);
    return o;
}

#endif /* COLOR_H */
