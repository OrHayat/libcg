#ifndef DRAW2D_H
#define DRAW2D_H

#include "platform/platform.h"

/* 2D primitives in screen space. Integer pixel coordinates, top-left
   origin. Everything is clipped to the framebuffer bounds. */

/* Callback receiving each pixel a rasterizer visits, in order. */
typedef void (*draw2d_pixel_fn)(int x, int y, void *user_data);

/* Bresenham walk from (x0,y0) to (x1,y1) inclusive, all octants. Calls
   fn once per pixel. No clipping — the caller decides what a pixel
   means (a framebuffer write, a brush stamp, ...). */
void draw2d_walk_line(int x0, int y0, int x1, int y1, draw2d_pixel_fn fn, void *user_data);

/* 1px line, endpoints inclusive, clipped per-pixel to fb. */
void draw2d_line(platform_framebuffer_t *fb, int x0, int y0, int x1, int y1, uint32_t color);

#endif /* DRAW2D_H */
