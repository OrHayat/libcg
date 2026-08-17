#include "util/image.h"
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

bool image_save_bmp(const char *path, const u32 *pixels, int w, int h) {
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
        const u32 *src = &pixels[(size_t)y * (size_t)w];
        for (int x = 0; x < w; x++) {
            u32 p = src[x];
            row[x * 3 + 0] = (u8)( p        & 0xFF);      /* B */
            row[x * 3 + 1] = (u8)((p >>  8) & 0xFF);      /* G */
            row[x * 3 + 2] = (u8)((p >> 16) & 0xFF);      /* R */
        }
        ok = fwrite(row, stride, 1, f) == 1;
    }

    free(row);
    if (fclose(f) != 0) ok = false;
    return ok;
}

bool image_load_bmp(const char *path, u32 **out_pixels, int *out_w, int *out_h) {
    if (!path || !out_pixels || !out_w || !out_h) return false;

    FILE *f = fopen(path, "rb");
    if (!f) return false;

    u8 hdr[BMP_FILE_HEADER + BMP_INFO_HEADER];
    if (fread(hdr, sizeof hdr, 1, f) != 1 || hdr[0] != 'B' || hdr[1] != 'M') {
        fclose(f);
        return false;
    }

    /* Width/height/bpp/compression sit at the same offsets in every info
       header version, and the pixel offset is read from the file header
       rather than assumed, so V4/V5 files still load. */
    u32 data_offset = get_u32(&hdr[10]);
    i32 width       = (i32)get_u32(&hdr[18]);
    i32 height      = (i32)get_u32(&hdr[22]);
    u16 bpp         = get_u16(&hdr[28]);
    u32 compression = get_u32(&hdr[30]);

    bool bottom_up = height > 0;              /* negative height = already top-down */
    if (height < 0) height = -height;

    if (width <= 0 || height <= 0 ||
        (bpp != 24 && bpp != 32) ||
        compression != 0) {                   /* BI_RGB only — no RLE, no bitfields */
        fclose(f);
        return false;
    }

    size_t bytes_per_px = bpp / 8;
    size_t stride       = (((size_t)width * bytes_per_px) + 3) & ~(size_t)3;

    u32 *pixels = malloc((size_t)width * (size_t)height * sizeof(u32));
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

        int  y   = bottom_up ? height - 1 - i : i;
        u32 *dst = &pixels[(size_t)y * (size_t)width];
        for (int x = 0; x < width; x++) {
            const u8 *p = &row[(size_t)x * bytes_per_px];
            /* Opaque even at 32bpp: under BI_RGB the fourth byte is
               declared unused, so a zero there means "no information",
               not "transparent". */
            dst[x] = 0xFF000000u | ((u32)p[2] << 16) | ((u32)p[1] << 8) | (u32)p[0];
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
