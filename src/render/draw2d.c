#include "render/draw2d.h"
#include "render/color.h"
#include "render/framebuffer.h"
#include <stdbool.h>
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

void draw2d_triangle_wire(platform_framebuffer_t *fb,
                          int x0, int y0, int x1, int y1, int x2, int y2, uint32_t color) {
    draw2d_line(fb, x0, y0, x1, y1, color);
    draw2d_line(fb, x1, y1, x2, y2, color);
    draw2d_line(fb, x2, y2, x0, y0, color);
}

/* Twice the signed area of triangle (a, b, p). Positive when p is on the
   left of a→b in screen space (y down). Inputs are pixel coordinates
   bounded by the framebuffer, so the products stay well inside int32. */
static int edge(int ax, int ay, int bx, int by, int px, int py) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

/* Top-left rule: with inside == (edge >= 0) and positive winding, an edge
   owns the pixels lying exactly on it when it is a "left" edge (points
   upward, dy < 0) or a "top" edge (horizontal, pointing right). Every
   other edge cedes them to the neighbouring triangle. */
static bool edge_is_top_left(int ax, int ay, int bx, int by) {
    int dx = bx - ax, dy = by - ay;
    return dy < 0 || (dy == 0 && dx > 0);
}

static int imin3(int a, int b, int c) { int m = a < b ? a : b; return m < c ? m : c; }
static int imax3(int a, int b, int c) { int m = a > b ? a : b; return m > c ? m : c; }

static void triangle_fill_impl(platform_framebuffer_t *fb,
                               int x0, int y0, int x1, int y1, int x2, int y2,
                               uint32_t color, bool blend) {
    int area = edge(x0, y0, x1, y1, x2, y2);
    if (area == 0) return;                      /* degenerate: no pixels */
    if (area < 0) {                             /* normalize to positive winding */
        int tx = x1, ty = y1;
        x1 = x2; y1 = y2;
        x2 = tx; y2 = ty;
    }

    int minx = imin3(x0, x1, x2), maxx = imax3(x0, x1, x2);
    int miny = imin3(y0, y1, y2), maxy = imax3(y0, y1, y2);
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx > fb->width  - 1) maxx = fb->width  - 1;
    if (maxy > fb->height - 1) maxy = fb->height - 1;

    /* Fold the fill rule into a per-edge bias so the inner test stays a
       plain sign check: on-edge pixels (w == 0) survive only where the
       edge owns them. */
    int bias0 = edge_is_top_left(x1, y1, x2, y2) ? 0 : -1;
    int bias1 = edge_is_top_left(x2, y2, x0, y0) ? 0 : -1;
    int bias2 = edge_is_top_left(x0, y0, x1, y1) ? 0 : -1;

    for (int y = miny; y <= maxy; y++) {
        for (int x = minx; x <= maxx; x++) {
            int w0 = edge(x1, y1, x2, y2, x, y) + bias0;
            int w1 = edge(x2, y2, x0, y0, x, y) + bias1;
            int w2 = edge(x0, y0, x1, y1, x, y) + bias2;
            if ((w0 | w1 | w2) >= 0) {          /* all three non-negative */
                uint32_t *px = &fb->pixels[y * fb->width + x];
                *px = blend ? color_blend(*px, color) : color;
            }
        }
    }
}

void draw2d_triangle_fill(platform_framebuffer_t *fb,
                          int x0, int y0, int x1, int y1, int x2, int y2, uint32_t color) {
    triangle_fill_impl(fb, x0, y0, x1, y1, x2, y2, color, false);
}

void draw2d_triangle_fill_blend(platform_framebuffer_t *fb,
                                int x0, int y0, int x1, int y1, int x2, int y2,
                                uint32_t premul_color) {
    triangle_fill_impl(fb, x0, y0, x1, y1, x2, y2, premul_color, true);
}
