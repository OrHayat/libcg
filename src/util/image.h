#ifndef IMAGE_H
#define IMAGE_H

#include "util/common.h"

/* Image files. Pixels are 0xAARRGGBB, top-left origin, row-major — the
   same layout as the framebuffer and the paint canvas. */

/* Writes a 24-bit uncompressed BMP. Alpha is dropped: BMP's 32-bit form
   declares its fourth byte "reserved", so readers disagree on whether it
   means transparency, and 24-bit is the variant every decoder agrees on.
   Returns false on any I/O error. */
bool image_save_bmp(const char *path, const u32 *pixels, int w, int h);

/* Reads an uncompressed 24- or 32-bit BMP. On success *out_pixels is a
   malloc'd buffer the caller owns and must free. */
bool image_load_bmp(const char *path, u32 **out_pixels, int *out_w, int *out_h);

#endif /* IMAGE_H */
