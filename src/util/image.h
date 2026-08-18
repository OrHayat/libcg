#ifndef IMAGE_H
#define IMAGE_H

#include "render/color.h"
#include "util/common.h"

/* Image files. Pixels are 0xAARRGGBB, top-left origin, row-major — the
   same layout as the framebuffer and the paint canvas. */

/* Writes a 24-bit uncompressed BMP. Alpha is dropped: BMP's 32-bit form
   declares its fourth byte "reserved", so readers disagree on whether it
   means transparency, and 24-bit is the variant every decoder agrees on.
   Source pixels are assumed premultiplied and opaque, which is what the
   framebuffer and the paint canvas hold. Returns false on any I/O error. */
bool image_save_bmp(const char *path, const pcolor_t *pixels, int w, int h);

/* Reads a 16-, 24- or 32-bit BMP, either BI_RGB or BI_BITFIELDS. Channel
   positions come from the file's own masks rather than an assumed byte
   order, because exporters disagree: GIMP and Photoshop routinely write
   32-bit BMPs as BI_BITFIELDS with an explicit alpha mask.

   Output is premultiplied 0xAARRGGBB, matching the framebuffer, so a
   decoded image can be blitted or blended without further conversion.
   Files with a colour palette (1/4/8 bpp) and RLE-compressed files are
   rejected. On success *out_pixels is a malloc'd buffer the caller owns. */
bool image_load_bmp(const char *path, pcolor_t **out_pixels, int *out_w, int *out_h);

#endif /* IMAGE_H */
