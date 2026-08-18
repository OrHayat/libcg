#include "util/image.h"
#include "render/color.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* BMP is little-endian whatever the host is, so fields are assembled byte
   by byte instead of fwrite'ing a struct — that also sidesteps any
   question of struct padding between the 14-byte file header and the
   40-byte info header. */
static void put_u16(u8 *p, u16 v) {
    p[0] = (u8)(v & 0xFF);
    p[1] = (u8)(v >> 8);
}

static void put_u32(u8 *p, u32 v) {
    p[0] = (u8)( v        & 0xFF);
    p[1] = (u8)((v >>  8) & 0xFF);
    p[2] = (u8)((v >> 16) & 0xFF);
    p[3] = (u8)((v >> 24) & 0xFF);
}

static u16 get_u16(const u8 *p) {
    return (u16)((u32)p[0] | ((u32)p[1] << 8));
}

static u32 get_u32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

#define BMP_FILE_HEADER 14
#define BMP_INFO_HEADER 40

bool image_save_bmp(const char *path, const pcolor_t *pixels, int w, int h) {
    if (!path || !pixels || w <= 0 || h <= 0) return false;

    /* Every row starts on a 4-byte boundary; at 3 bytes per pixel that
       usually means trailing pad bytes. */
    size_t row_bytes   = (size_t)w * 3;
    size_t padding     = (4 - (row_bytes % 4)) % 4;
    size_t stride      = row_bytes + padding;
    size_t pixel_bytes = stride * (size_t)h;

    u8 hdr[BMP_FILE_HEADER + BMP_INFO_HEADER] = {0};
    hdr[0] = 'B';
    hdr[1] = 'M';
    put_u32(&hdr[2],  (u32)(sizeof hdr + pixel_bytes));   /* total file size */
    put_u32(&hdr[10], (u32)sizeof hdr);                   /* offset to pixels */
    put_u32(&hdr[14], BMP_INFO_HEADER);
    put_u32(&hdr[18], (u32)w);
    put_u32(&hdr[22], (u32)h);                            /* positive = bottom-up */
    put_u16(&hdr[26], 1);                                 /* colour planes */
    put_u16(&hdr[28], 24);                                /* bits per pixel */
    put_u32(&hdr[30], 0);                                 /* BI_RGB, uncompressed */
    put_u32(&hdr[34], (u32)pixel_bytes);
    put_u32(&hdr[38], 2835);                              /* ~72 DPI in px/metre */
    put_u32(&hdr[42], 2835);

    FILE *f = fopen(path, "wb");
    if (!f) return false;

    u8 *row = calloc(1, stride);                          /* pad bytes stay zero */
    if (!row) {
        fclose(f);
        return false;
    }

    bool ok = fwrite(hdr, sizeof hdr, 1, f) == 1;
    /* BMP's first stored row is the image's bottom row. */
    for (int y = h - 1; y >= 0 && ok; y--) {
        const pcolor_t *src = &pixels[(size_t)y * (size_t)w];
        for (int x = 0; x < w; x++) {
            row[x * 3 + 0] = src[x].b;
            row[x * 3 + 1] = src[x].g;
            row[x * 3 + 2] = src[x].r;
        }
        ok = fwrite(row, stride, 1, f) == 1;
    }

    free(row);
    if (fclose(f) != 0) ok = false;
    return ok;
}

/* A channel described by its mask: where its bits sit and how many there
   are. BMP stores these explicitly for BI_BITFIELDS files, which is the
   only reliable way to know the byte order — exporters disagree. */
typedef struct {
    u32 mask;
    int shift;
    int bits;
} channel_t;

static channel_t channel_from_mask(u32 mask) {
    channel_t c = { mask, 0, 0 };
    if (!mask) return c;
    while (!((mask >> c.shift) & 1u)) c.shift++;
    for (u32 m = mask >> c.shift; m & 1u; m >>= 1) c.bits++;
    return c;
}

/* Scale a channel of arbitrary width up to 8 bits: a 5-bit 31 becomes 255,
   not 248, so white stays white. */
static u8 channel_extract(const channel_t *c, u32 px) {
    if (!c->mask || c->bits <= 0) return 0;
    u32 v    = (px & c->mask) >> c->shift;
    u32 maxv = (c->bits >= 32) ? 0xFFFFFFFFu : ((1u << c->bits) - 1u);
    return (u8)((v * 255u + maxv / 2u) / maxv);
}

/* Assemble one little-endian pixel of `bytes` width. */
static u32 read_px(const u8 *p, size_t bytes) {
    u32 v = 0;
    for (size_t i = 0; i < bytes; i++) v |= (u32)p[i] << (8u * i);
    return v;
}

#define BMP_INFO_HEADER_MAX 124   /* BITMAPV5HEADER */

bool image_load_bmp(const char *path, pcolor_t **out_pixels, int *out_w, int *out_h) {
    if (!path || !out_pixels || !out_w || !out_h) return false;

    FILE *f = fopen(path, "rb");
    if (!f) return false;

    u8 fh[BMP_FILE_HEADER];
    if (fread(fh, sizeof fh, 1, f) != 1 || fh[0] != 'B' || fh[1] != 'M') {
        fclose(f);
        return false;
    }
    u32 data_offset = get_u32(&fh[10]);

    /* The info header's length tells us which version it is, and therefore
       whether the channel masks live inside it or follow it in the file. */
    u8 ih[BMP_INFO_HEADER_MAX] = {0};
    if (fread(ih, 4, 1, f) != 1) {
        fclose(f);
        return false;
    }
    u32 ih_size = get_u32(&ih[0]);
    if (ih_size < BMP_INFO_HEADER || ih_size > BMP_INFO_HEADER_MAX) {
        fclose(f);
        return false;
    }
    if (fread(&ih[4], ih_size - 4, 1, f) != 1) {
        fclose(f);
        return false;
    }

    i32 width       = (i32)get_u32(&ih[4]);
    i32 height      = (i32)get_u32(&ih[8]);
    u16 bpp         = get_u16(&ih[14]);
    u32 compression = get_u32(&ih[16]);

    bool bottom_up = height > 0;              /* negative height = already top-down */
    if (height < 0) height = -height;

    if (width <= 0 || height <= 0) {
        fclose(f);
        return false;
    }
    if (bpp != 16 && bpp != 24 && bpp != 32) {   /* palette formats unsupported */
        fclose(f);
        return false;
    }
    if (compression != 0 && compression != 3) {  /* no RLE */
        fclose(f);
        return false;
    }

    u32 mask_r = 0, mask_g = 0, mask_b = 0, mask_a = 0;
    if (compression == 3) {
        /* V4 and later carry the masks in the header; a plain 40-byte
           header stores them in the 12 bytes immediately after it. */
        if (ih_size >= 56) {
            mask_r = get_u32(&ih[40]);
            mask_g = get_u32(&ih[44]);
            mask_b = get_u32(&ih[48]);
            mask_a = get_u32(&ih[52]);
        } else if (ih_size >= 52) {
            mask_r = get_u32(&ih[40]);
            mask_g = get_u32(&ih[44]);
            mask_b = get_u32(&ih[48]);
        } else {
            u8 m[12];
            if (fread(m, sizeof m, 1, f) != 1) {
                fclose(f);
                return false;
            }
            mask_r = get_u32(&m[0]);
            mask_g = get_u32(&m[4]);
            mask_b = get_u32(&m[8]);
        }
        if (!mask_r && !mask_g && !mask_b) {
            fclose(f);
            return false;
        }
    } else {
        /* BI_RGB has fixed layouts. At 32bpp the top byte is declared
           unused, so no alpha mask — a zero there means "no information",
           not "transparent". */
        if (bpp == 16) {
            mask_r = 0x7C00u; mask_g = 0x03E0u; mask_b = 0x001Fu;   /* 5-5-5 */
        } else {
            mask_r = 0x00FF0000u; mask_g = 0x0000FF00u; mask_b = 0x000000FFu;
        }
    }

    channel_t cr = channel_from_mask(mask_r);
    channel_t cg = channel_from_mask(mask_g);
    channel_t cb = channel_from_mask(mask_b);
    channel_t ca = channel_from_mask(mask_a);

    size_t bytes_per_px = bpp / 8u;
    size_t stride       = (((size_t)width * bytes_per_px) + 3u) & ~(size_t)3;

    pcolor_t *pixels = malloc((size_t)width * (size_t)height * sizeof *pixels);
    u8  *row    = malloc(stride);
    if (!pixels || !row) {
        free(pixels);
        free(row);
        fclose(f);
        return false;
    }

    bool ok = fseek(f, (long)data_offset, SEEK_SET) == 0;
    for (int i = 0; i < height && ok; i++) {
        ok = fread(row, stride, 1, f) == 1;
        if (!ok) break;

        int       y   = bottom_up ? height - 1 - i : i;
        pcolor_t *dst = &pixels[(size_t)y * (size_t)width];
        for (int x = 0; x < width; x++) {
            u32 px = read_px(&row[(size_t)x * bytes_per_px], bytes_per_px);
            u8  r  = channel_extract(&cr, px);
            u8  g  = channel_extract(&cg, px);
            u8  b  = channel_extract(&cb, px);
            u8  a  = ca.mask ? channel_extract(&ca, px) : 0xFF;
            /* Premultiply so the result can be blitted or blended straight
               into the framebuffer. */
            dst[x] = color_premultiply(COLOR_RGBA(r, g, b, a));
        }
    }

    free(row);
    fclose(f);
    if (!ok) {
        free(pixels);
        return false;
    }

    *out_pixels = pixels;
    *out_w      = width;
    *out_h      = height;
    return true;
}
