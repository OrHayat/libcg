#ifndef DRAW2D_H
#define DRAW2D_H

#include "platform/platform.h"
#include "render/color.h"
#include "render/geom.h"

/* 2D primitives in screen space. Integer pixel coordinates, top-left
   origin. Everything is clipped to the framebuffer bounds. */

/* A corner with its shading attributes attached. Position lives in vec2_t
   because most points in the codebase carry no colour at all; a vertex is
   the thing that does. Later attributes (depth, UV) attach here too, which
   is why this is a separate type rather than a fatter point. */
typedef struct {
    vec2_t   pos;
    pcolor_t color;      /* premultiplied — see the interpolation note below */
} vertex2d_t;

/* Called once for each pixel a rasterizer visits, in order. user_data is
   passed straight back untouched: C has no closures, so it is how a caller
   gets its own state into the callback. */
typedef void (*draw2d_pixel_fn)(int x, int y, void *user_data);

/* Bresenham walk from l.a to l.b inclusive, all octants. Calls on_pixel
   once per pixel. No clipping — the caller decides what a pixel means (a framebuffer
   write, a brush stamp, ...). */
void draw2d_walk_line(line2d_t l, draw2d_pixel_fn on_pixel, void *user_data);

/* Every pixel a filled triangle would cover, reported to on_pixel in raster order
   under the same top-left rule the fills use, clipped to `clip`. The target
   need not be a framebuffer — paint marks a canvas-sized coverage mask.

   Returns false when it covered nothing, which a caller drawing a live
   preview needs to distinguish: collinear corners have zero area and so no
   interior at all, and silently drawing nothing makes a rubber-band vanish
   rather than degenerate to the line it actually is. */
bool draw2d_walk_triangle(tri2d_t t, rect2d_t clip,
                          draw2d_pixel_fn on_pixel, void *user_data);

/* 1px line, endpoints inclusive, clipped per-pixel to fb. */
void draw2d_line(platform_framebuffer_t *fb, line2d_t l, pcolor_t color);

/* Triangle outline: three draw2d_line calls. */
void draw2d_triangle_wire(platform_framebuffer_t *fb, tri2d_t t, pcolor_t color);

/* Solid triangle via edge functions over the bounding box.
   Winding-agnostic (vertices are reordered internally to positive area),
   with a top-left fill rule so two triangles sharing an edge cover the
   shared pixels exactly once — no seams, no double-blend. The three edge
   values computed per pixel are the unnormalized barycentric weights that
   draw2d_triangle_fill_gradient interpolates with. */
void draw2d_triangle_fill(platform_framebuffer_t *fb, tri2d_t t, pcolor_t color);

/* Same coverage, but source-over blends instead of replacing. Use this
   whenever the colour may be translucent: the plain fill above writes the
   premultiplied value straight in, which replaces the background rather
   than showing through it. */
void draw2d_triangle_fill_blend(platform_framebuffer_t *fb, tri2d_t t, pcolor_t color);

/* Triangle with a colour per corner, blended smoothly across the interior.
   Known formally as Gouraud shading, after the 1971 paper; named for what
   it does here because that is what the call site needs to convey.
   Coverage and the top-left fill rule are identical to
   draw2d_triangle_fill_blend — only the colour varies per pixel.

   The weights are the three edge functions normalized by the triangle's
   area, so a pixel two thirds of the way to corner b gets two thirds of
   b's colour. Interpolation happens in PREMULTIPLIED space, which is the
   representation that survives it: blending straight-alpha colours would
   let a transparent corner drag its RGB into its neighbours.

   Accumulates in i64. The weights scale with triangle AREA, so a
   full-screen triangle times a 255 channel overflows i32. */
void draw2d_triangle_fill_gradient(platform_framebuffer_t *fb,
                                   vertex2d_t a, vertex2d_t b, vertex2d_t c);

#endif /* DRAW2D_H */
