#include "render/draw2d.h"
#include "render/framebuffer.h"
#include <stdlib.h>

/* Integer-only Bresenham. Steps one pixel per iteration along the major
   axis; `err` accumulates the minor-axis error in units of 2*d so no
   division or float is needed. Symmetric across octants because sx/sy
   carry the direction and dx/dy the magnitudes. */
void draw2d_walk_line(int x0, int y0, int x1, int y1, draw2d_pixel_fn fn, void *user_data) {
    int dx =  abs(x1 - x0);
    int dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        fn(x0, y0, user_data);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

typedef struct {
    platform_framebuffer_t *fb;
    uint32_t                color;
} line_ctx_t;

static void put_pixel(int x, int y, void *ud) {
    line_ctx_t *c = ud;
    framebuffer_set_pixel(c->fb, x, y, c->color);
}

/* Per-pixel clipping via framebuffer_set_pixel is enough for now: lines
   come from on-screen geometry (paint strokes, later NDC-clipped
   projections), so mostly-offscreen lines are rare and the walk cost
   is bounded by max(|dx|,|dy|). Proper Cohen-Sutherland is a follow-up
   if that stops being true. */
void draw2d_line(platform_framebuffer_t *fb, int x0, int y0, int x1, int y1, uint32_t color) {
    line_ctx_t c = { .fb = fb, .color = color };
    draw2d_walk_line(x0, y0, x1, y1, put_pixel, &c);
}
