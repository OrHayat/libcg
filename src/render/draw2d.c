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
    pcolor_t                color;
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
void draw2d_line(platform_framebuffer_t *fb, int x0, int y0, int x1, int y1, pcolor_t color) {
    line_ctx_t c = { .fb = fb, .color = color };
    draw2d_walk_line(x0, y0, x1, y1, put_pixel, &c);
}

void draw2d_triangle_wire(platform_framebuffer_t *fb,
                          int x0, int y0, int x1, int y1, int x2, int y2, pcolor_t color) {
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

/* Geometry shared by every triangle traversal: winding normalized to
   positive, bounding box clipped, and the three edge functions seeded at
   the box's top-left corner with their top-left-rule biases folded in.
   One definition so the coverage a fill produces and the coverage a walk
   reports can never drift apart. Returns false when nothing is covered. */
typedef struct {
    int minx, miny, maxx, maxy;
    int dx0, dy0, dx1, dy1, dx2, dy2;
    int row0, row1, row2;
} tri_span_t;

static bool tri_span_setup(int x0, int y0, int x1, int y1, int x2, int y2,
                           int clip_w, int clip_h, tri_span_t *s) {
    int area = edge(x0, y0, x1, y1, x2, y2);
    if (area == 0) return false;                /* degenerate: no pixels */
    if (area < 0) {                             /* normalize to positive winding */
        int tx = x1, ty = y1;
        x1 = x2; y1 = y2;
        x2 = tx; y2 = ty;
    }

    s->minx = imin3(x0, x1, x2); s->maxx = imax3(x0, x1, x2);
    s->miny = imin3(y0, y1, y2); s->maxy = imax3(y0, y1, y2);
    if (s->minx < 0) s->minx = 0;
    if (s->miny < 0) s->miny = 0;
    if (s->maxx > clip_w - 1) s->maxx = clip_w - 1;
    if (s->maxy > clip_h - 1) s->maxy = clip_h - 1;
    if (s->minx > s->maxx || s->miny > s->maxy) return false;

    /* Fold the fill rule into a per-edge bias so the inner test stays a
       plain sign check: on-edge pixels (w == 0) survive only where the
       edge owns them. */
    int bias0 = edge_is_top_left(x1, y1, x2, y2) ? 0 : -1;
    int bias1 = edge_is_top_left(x2, y2, x0, y0) ? 0 : -1;
    int bias2 = edge_is_top_left(x0, y0, x1, y1) ? 0 : -1;

    /* An edge function is linear in the pixel coordinate, so one step moves
       it by a constant: d/dx is -(by - ay), d/dy is (bx - ax). Evaluate the
       three edges once at the bounding-box corner and add those deltas per
       step — 3 adds per pixel instead of 6 multiplies, which measured 2.3x
       faster on a half-screen opaque fill. The biases fold in at the corner
       and ride along unchanged. */
    s->dx0 = y1 - y2; s->dy0 = x2 - x1;
    s->dx1 = y2 - y0; s->dy1 = x0 - x2;
    s->dx2 = y0 - y1; s->dy2 = x1 - x0;

    s->row0 = edge(x1, y1, x2, y2, s->minx, s->miny) + bias0;
    s->row1 = edge(x2, y2, x0, y0, s->minx, s->miny) + bias1;
    s->row2 = edge(x0, y0, x1, y1, s->minx, s->miny) + bias2;
    return true;
}

static void triangle_fill_impl(platform_framebuffer_t *fb,
                               int x0, int y0, int x1, int y1, int x2, int y2,
                               pcolor_t color, bool blend) {
    tri_span_t s;
    if (!tri_span_setup(x0, y0, x1, y1, x2, y2, fb->width, fb->height, &s)) return;

    /* Lifted into locals so the inner loops stay exactly the arithmetic the
       vectorizer handled before this setup was shared. */
    const int minx = s.minx, maxx = s.maxx;
    const int dx0 = s.dx0, dx1 = s.dx1, dx2 = s.dx2;
    const int dy0 = s.dy0, dy1 = s.dy1, dy2 = s.dy2;
    int row0 = s.row0, row1 = s.row1, row2 = s.row2;

    /* Hoisted out of the loop on purpose: a uint32_t store may alias the
       int members of platform_framebuffer_t, so with fb coming from another
       translation unit the compiler has to reload fb->pixels and fb->width
       after every pixel it writes. Those reloads, not the edge math,
       dominated the loop. */
    pcolor_t *pixels = pcolor_pixels(fb->pixels);
    int       stride = fb->width;

    /* The blend test is hoisted out of the loop rather than sitting in it.
       Kept inside, it is a runtime branch the vectorizer will not cross, so
       the loop stays one pixel per iteration; hoisted, the opaque loop is a
       plain conditional store and clang does the edge math four pixels at a
       time (2.7x on a half-screen fill). */
    if (blend) {
        for (int y = s.miny; y <= s.maxy; y++) {
            int w0 = row0, w1 = row1, w2 = row2;
            pcolor_t *px = &pixels[(size_t)y * (size_t)stride];
            for (int x = minx; x <= maxx; x++) {
                if ((w0 | w1 | w2) >= 0)        /* all three non-negative */
                    px[x] = color_blend(px[x], color);
                w0 += dx0; w1 += dx1; w2 += dx2;
            }
            row0 += dy0; row1 += dy1; row2 += dy2;
        }
    } else {
        for (int y = s.miny; y <= s.maxy; y++) {
            int w0 = row0, w1 = row1, w2 = row2;
            pcolor_t *px = &pixels[(size_t)y * (size_t)stride];
            for (int x = minx; x <= maxx; x++) {
                if ((w0 | w1 | w2) >= 0)
                    px[x] = color;
                w0 += dx0; w1 += dx1; w2 += dx2;
            }
            row0 += dy0; row1 += dy1; row2 += dy2;
        }
    }
}

/* Same coverage as the fills, reported instead of written. Takes explicit
   clip bounds because the target need not be a framebuffer — paint marks a
   canvas-sized coverage mask with it. */
bool draw2d_walk_triangle(int x0, int y0, int x1, int y1, int x2, int y2,
                          int clip_w, int clip_h, draw2d_pixel_fn fn, void *user_data) {
    tri_span_t s;
    if (!tri_span_setup(x0, y0, x1, y1, x2, y2, clip_w, clip_h, &s)) return false;
    bool any = false;
    for (int y = s.miny; y <= s.maxy; y++) {
        int w0 = s.row0, w1 = s.row1, w2 = s.row2;
        for (int x = s.minx; x <= s.maxx; x++) {
            if ((w0 | w1 | w2) >= 0) { fn(x, y, user_data); any = true; }
            w0 += s.dx0; w1 += s.dx1; w2 += s.dx2;
        }
        s.row0 += s.dy0; s.row1 += s.dy1; s.row2 += s.dy2;
    }
    return any;
}

void draw2d_triangle_fill(platform_framebuffer_t *fb,
                          int x0, int y0, int x1, int y1, int x2, int y2, pcolor_t color) {
    triangle_fill_impl(fb, x0, y0, x1, y1, x2, y2, color, false);
}

void draw2d_triangle_fill_blend(platform_framebuffer_t *fb,
                                int x0, int y0, int x1, int y1, int x2, int y2,
                                pcolor_t color) {
    triangle_fill_impl(fb, x0, y0, x1, y1, x2, y2, color, true);
}
