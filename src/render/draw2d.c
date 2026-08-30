#include "render/draw2d.h"
#include "render/color.h"
#include "render/framebuffer.h"
#include <stdbool.h>
#include <stdlib.h>

/* Integer-only Bresenham. Steps one pixel per iteration along the major
   axis; `err` accumulates the minor-axis error in units of 2*d so no
   division or float is needed. Symmetric across octants because sx/sy
   carry the direction and dx/dy the magnitudes. */
void draw2d_walk_line(line2d_t l, draw2d_pixel_fn on_pixel, void *user_data) {
    int x = l.a.x, y = l.a.y;
    int dx =  abs(l.b.x - x);
    int dy = -abs(l.b.y - y);
    int sx = x < l.b.x ? 1 : -1;
    int sy = y < l.b.y ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        on_pixel(x, y, user_data);
        if (x == l.b.x && y == l.b.y) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x += sx; }
        if (e2 <= dx) { err += dx; y += sy; }
    }
}

typedef struct {
    platform_framebuffer_t *fb;
    pcolor_t                color;
} line_ctx_t;

static void put_pixel(int x, int y, void *user_data) {
    line_ctx_t *c = user_data;
    framebuffer_set_pixel(c->fb, x, y, c->color);
}

/* Per-pixel clipping via framebuffer_set_pixel is enough for now: lines
   come from on-screen geometry (paint strokes, later NDC-clipped
   projections), so mostly-offscreen lines are rare and the walk cost
   is bounded by max(|dx|,|dy|). Proper Cohen-Sutherland is a follow-up
   if that stops being true. */
void draw2d_line(platform_framebuffer_t *fb, line2d_t l, pcolor_t color) {
    line_ctx_t c = { .fb = fb, .color = color };
    draw2d_walk_line(l, put_pixel, &c);
}

void draw2d_triangle_wire(platform_framebuffer_t *fb, tri2d_t t, pcolor_t color) {
    draw2d_line(fb, line2d(t.a, t.b), color);
    draw2d_line(fb, line2d(t.b, t.c), color);
    draw2d_line(fb, line2d(t.c, t.a), color);
}

/* Top-left rule: with inside == (edge >= 0) and positive winding, an edge
   owns the pixels lying exactly on it when it is a "left" edge (points
   upward, dy < 0) or a "top" edge (horizontal, pointing right). Every
   other edge cedes them to the neighbouring triangle. */
static bool edge_is_top_left(vec2_t a, vec2_t b) {
    vec2_t d = vec2_sub(b, a);
    return d.y < 0 || (d.y == 0 && d.x > 0);
}

/* Traversal state shared by every triangle rasterizer here, so the coverage
   a fill produces and the coverage a walk reports can never drift apart.

   Each w_* is one edge function, named for the edge it measures: w_bc is
   the edge b->c, whose value is the barycentric weight of the OPPOSITE
   corner, a. Reading a negative w_bc tells you directly which edge the
   pixel fell outside of. */
typedef struct {
    rect2d_t box;                        /* bounding box, clipped to the target */

    /* An edge function is linear in the pixel coordinate, so one step moves
       it by a constant: d/dx is -(by - ay), d/dy is (bx - ax). Evaluate the
       three edges once at the box corner and add these per step — 3 adds
       per pixel instead of 6 multiplies, which measured 2.3x faster on a
       half-screen opaque fill. */
    int w_bc_dx, w_bc_dy;
    int w_ca_dx, w_ca_dy;
    int w_ab_dx, w_ab_dy;

    int w_bc_row, w_ca_row, w_ab_row;    /* value at the box's left edge, biased */
    int bias_bc, bias_ca, bias_ab;       /* subtract to recover the true weight */
} tri_span_t;

static bool tri_span_setup(tri2d_t t, rect2d_t clip, tri_span_t *s) {
    t = tri2d_to_positive(t);
    if (tri2d_area2(t) == 0) return false;          /* degenerate: no pixels */

    s->box = rect2d_intersect(tri2d_bounds(t), clip);
    if (rect2d_is_empty(s->box)) return false;

    /* Fold the fill rule into a per-edge bias so the inner test stays a
       plain sign check: on-edge pixels (w == 0) survive only where the
       edge owns them. The biases fold in at the corner and ride along
       unchanged as the values step. */
    s->bias_bc = edge_is_top_left(t.b, t.c) ? 0 : -1;
    s->bias_ca = edge_is_top_left(t.c, t.a) ? 0 : -1;
    s->bias_ab = edge_is_top_left(t.a, t.b) ? 0 : -1;

    s->w_bc_dx = t.b.y - t.c.y;  s->w_bc_dy = t.c.x - t.b.x;
    s->w_ca_dx = t.c.y - t.a.y;  s->w_ca_dy = t.a.x - t.c.x;
    s->w_ab_dx = t.a.y - t.b.y;  s->w_ab_dy = t.b.x - t.a.x;

    vec2_t corner = vec2(s->box.x, s->box.y);
    s->w_bc_row = edge2d_side(t.b, t.c, corner) + s->bias_bc;
    s->w_ca_row = edge2d_side(t.c, t.a, corner) + s->bias_ca;
    s->w_ab_row = edge2d_side(t.a, t.b, corner) + s->bias_ab;
    return true;
}

static void triangle_fill_impl(platform_framebuffer_t *fb, tri2d_t t,
                               pcolor_t color, bool blend) {
    tri_span_t s;
    if (!tri_span_setup(t, rect2d(0, 0, fb->width, fb->height), &s)) return;

    /* Lifted into locals so the inner loops stay exactly the arithmetic the
       vectorizer handled before this setup was shared. */
    const int x0 = s.box.x, x1 = s.box.x + s.box.w - 1;
    const int y0 = s.box.y, y1 = s.box.y + s.box.h - 1;
    const int bc_dx = s.w_bc_dx, ca_dx = s.w_ca_dx, ab_dx = s.w_ab_dx;
    const int bc_dy = s.w_bc_dy, ca_dy = s.w_ca_dy, ab_dy = s.w_ab_dy;
    int bc_row = s.w_bc_row, ca_row = s.w_ca_row, ab_row = s.w_ab_row;

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
        for (int y = y0; y <= y1; y++) {
            int w_bc = bc_row, w_ca = ca_row, w_ab = ab_row;
            pcolor_t *px = &pixels[(size_t)y * (size_t)stride];
            for (int x = x0; x <= x1; x++) {
                if ((w_bc | w_ca | w_ab) >= 0)   /* all three non-negative */
                    px[x] = color_blend(px[x], color);
                w_bc += bc_dx; w_ca += ca_dx; w_ab += ab_dx;
            }
            bc_row += bc_dy; ca_row += ca_dy; ab_row += ab_dy;
        }
    } else {
        for (int y = y0; y <= y1; y++) {
            int w_bc = bc_row, w_ca = ca_row, w_ab = ab_row;
            pcolor_t *px = &pixels[(size_t)y * (size_t)stride];
            for (int x = x0; x <= x1; x++) {
                if ((w_bc | w_ca | w_ab) >= 0)
                    px[x] = color;
                w_bc += bc_dx; w_ca += ca_dx; w_ab += ab_dx;
            }
            bc_row += bc_dy; ca_row += ca_dy; ab_row += ab_dy;
        }
    }
}

bool draw2d_walk_triangle(tri2d_t t, rect2d_t clip,
                          draw2d_pixel_fn on_pixel, void *user_data) {
    tri_span_t s;
    if (!tri_span_setup(t, clip, &s)) return false;

    bool any = false;
    for (int y = s.box.y; y < s.box.y + s.box.h; y++) {
        int w_bc = s.w_bc_row, w_ca = s.w_ca_row, w_ab = s.w_ab_row;
        for (int x = s.box.x; x < s.box.x + s.box.w; x++) {
            if ((w_bc | w_ca | w_ab) >= 0) { on_pixel(x, y, user_data); any = true; }
            w_bc += s.w_bc_dx; w_ca += s.w_ca_dx; w_ab += s.w_ab_dx;
        }
        s.w_bc_row += s.w_bc_dy; s.w_ca_row += s.w_ca_dy; s.w_ab_row += s.w_ab_dy;
    }
    return any;
}

void draw2d_triangle_fill(platform_framebuffer_t *fb, tri2d_t t, pcolor_t color) {
    triangle_fill_impl(fb, t, color, false);
}

void draw2d_triangle_fill_blend(platform_framebuffer_t *fb, tri2d_t t, pcolor_t color) {
    triangle_fill_impl(fb, t, color, true);
}

void draw2d_triangle_fill_gradient(platform_framebuffer_t *fb,
                                   vertex2d_t a, vertex2d_t b, vertex2d_t c) {
    /* Normalize winding HERE rather than leaving it to tri_span_setup: the
       colours have to travel with their corners, and a swap hidden inside
       setup would pair each weight with the wrong vertex. */
    if (edge2d_side(a.pos, b.pos, c.pos) < 0) { vertex2d_t t = b; b = c; c = t; }

    tri_span_t s;
    if (!tri_span_setup(tri2d(a.pos, b.pos, c.pos),
                        rect2d(0, 0, fb->width, fb->height), &s)) return;

    /* The three unbiased weights sum to twice the area at every pixel, so
       the divisor is a constant rather than a per-pixel sum. */
    const i64 total = (i64)edge2d_side(a.pos, b.pos, c.pos);
    if (total == 0) return;

    pcolor_t *pixels = pcolor_pixels(fb->pixels);
    int       stride = fb->width;

    for (int y = s.box.y; y < s.box.y + s.box.h; y++) {
        int w_bc = s.w_bc_row, w_ca = s.w_ca_row, w_ab = s.w_ab_row;
        pcolor_t *px = &pixels[(size_t)y * (size_t)stride];
        for (int x = s.box.x; x < s.box.x + s.box.w; x++) {
            if ((w_bc | w_ca | w_ab) >= 0) {
                /* coverage used the biased values; interpolation needs the
                   true weights, so the biases come back out here */
                i64 wa = w_bc - s.bias_bc;   /* weight of corner a */
                i64 wb = w_ca - s.bias_ca;   /* weight of corner b */
                i64 wc = w_ab - s.bias_ab;   /* weight of corner c */
                pcolor_t src;
                src.r = (u8)((wa*a.color.r + wb*b.color.r + wc*c.color.r) / total);
                src.g = (u8)((wa*a.color.g + wb*b.color.g + wc*c.color.g) / total);
                src.b = (u8)((wa*a.color.b + wb*b.color.b + wc*c.color.b) / total);
                src.a = (u8)((wa*a.color.a + wb*b.color.a + wc*c.color.a) / total);
                px[x] = color_blend(px[x], src);
            }
            w_bc += s.w_bc_dx; w_ca += s.w_ca_dx; w_ab += s.w_ab_dx;
        }
        s.w_bc_row += s.w_bc_dy; s.w_ca_row += s.w_ca_dy; s.w_ab_row += s.w_ab_dy;
    }
}
