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

/* Triangle outline: three draw2d_line calls. */
void draw2d_triangle_wire(platform_framebuffer_t *fb,
                          int x0, int y0, int x1, int y1, int x2, int y2, uint32_t color);

/* Solid triangle via edge functions over the bounding box.
   Winding-agnostic (vertices are reordered internally to positive area),
   with a top-left fill rule so two triangles sharing an edge cover the
   shared pixels exactly once — no seams, no double-blend. The three edge
   values computed per pixel are the unnormalized barycentric weights that
   later phases interpolate depth / color / UV with. */
void draw2d_triangle_fill(platform_framebuffer_t *fb,
                          int x0, int y0, int x1, int y1, int x2, int y2, uint32_t color);

/* Same coverage, but source-over blends instead of replacing. `color` must
   already be premultiplied, as must the destination. */
void draw2d_triangle_fill_blend(platform_framebuffer_t *fb,
                                int x0, int y0, int x1, int y1, int x2, int y2,
                                uint32_t premul_color);

#endif /* DRAW2D_H */
