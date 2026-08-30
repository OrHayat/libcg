#ifndef GEOM_H
#define GEOM_H

/* Only bool and int are used here; the u8/u32 typedefs in util/common.h
   would be an unused dependency for what is meant to be a leaf header. */
#include <stdbool.h>

/* ============================================================
   Integer 2D geometry in screen space.

   Coordinates are whole pixels: origin top-left, +x right, +y DOWN,
   matching the framebuffer's row-major layout. Integer rather than float
   throughout because every consumer rasterizes to a pixel grid, and
   exact integer arithmetic is what lets two triangles sharing an edge
   agree on that edge to the pixel — see the top-left rule in draw2d.c.

   A coordinate pair is one value, not two loose ints. Passing x and y
   separately is how a call ends up transposed with no compiler complaint,
   and how "clear the pending shape" becomes "remember to reset six
   fields" instead of one assignment.

   OVERFLOW: products below are of coordinate DIFFERENCES, so for a
   framebuffer under ~32k on a side they stay far inside i32. Scaling
   coordinates up (sub-pixel sampling for antialiasing multiplies each
   operand, so 8x scaling grows a product 64x) is the case that needs
   i64 instead.
   ============================================================ */

/* A point, or the vector between two points — the distinction is in how
   you use it, not in the bits. */
typedef struct { int x, y; } vec2_t;

/* A directed segment. Both endpoints are INCLUSIVE: a rasterizer walking
   this visits a and b themselves. Direction matters for edge2d_side. */
typedef struct { vec2_t a, b; } line2d_t;

/* Three corners in no particular winding — callers that need positive
   area normalize with tri2d_area2 rather than demanding an order. */
typedef struct { vec2_t a, b, c; } tri2d_t;

/* Axis-aligned rectangle. w or h of 0 means empty, which is why the
   bounds helpers below use exclusive right/bottom edges. */
typedef struct { int x, y, w, h; } rect2d_t;

static inline vec2_t   vec2(int x, int y)                 { return (vec2_t){ x, y }; }
static inline line2d_t line2d(vec2_t a, vec2_t b)         { return (line2d_t){ a, b }; }
static inline tri2d_t  tri2d(vec2_t a, vec2_t b, vec2_t c){ return (tri2d_t){ a, b, c }; }
static inline rect2d_t rect2d(int x, int y, int w, int h) { return (rect2d_t){ x, y, w, h }; }

static inline vec2_t vec2_add(vec2_t a, vec2_t b) { return vec2(a.x + b.x, a.y + b.y); }
static inline vec2_t vec2_sub(vec2_t a, vec2_t b) { return vec2(a.x - b.x, a.y - b.y); }
static inline bool   vec2_eq (vec2_t a, vec2_t b) { return a.x == b.x && a.y == b.y; }

/* ---- the one primitive everything else is built from ----

   The 2D cross product: the z of (a x b) if both were 3D vectors lying
   in the z=0 plane. Its magnitude is twice the area of the triangle the
   two vectors span, and its SIGN says which way b turns relative to a.
   Side-of-line tests, triangle winding and area are all this one call. */
static inline int vec2_cross(vec2_t a, vec2_t b) {
    return a.x * b.y - a.y * b.x;
}

/* Which side of the directed line a->b does p fall on?

   Returns twice the signed area of triangle (a, b, p):
     > 0   p is clockwise from a->b AS DRAWN ON SCREEN
     = 0   p is exactly on the infinite line through a and b
     < 0   p is counter-clockwise

   Clockwise, not counter-clockwise, because y grows downward here. The
   textbook "positive means left" assumes y grows upward; flipping y
   flips the handedness and so flips this sign. Concretely, with a->b
   pointing right, a p BELOW the line gives a positive result.

   Magnitude grows linearly with distance from the line, which is what
   lets a rasterizer step this value per pixel with a constant add
   instead of recomputing the multiply. */
static inline int edge2d_side(vec2_t a, vec2_t b, vec2_t p) {
    return vec2_cross(vec2_sub(b, a), vec2_sub(p, a));
}

/* Twice the signed area. Zero means degenerate: the three corners are
   collinear, so the triangle has no interior and covers no pixels at
   all, however far apart the corners are. Sign is the winding, read as
   for edge2d_side. */
static inline int tri2d_area2(tri2d_t t) {
    return edge2d_side(t.a, t.b, t.c);
}

/* Corners reordered to positive (clockwise-on-screen) winding, leaving
   the shape identical. Rasterizers want a known sign so the three side
   tests can share one comparison. A degenerate triangle is returned
   unchanged — there is no winding to fix. */
static inline tri2d_t tri2d_to_positive(tri2d_t t) {
    if (tri2d_area2(t) < 0) { vec2_t s = t.b; t.b = t.c; t.c = s; }
    return t;
}

/* ---- rectangles ---- */

static inline bool rect2d_is_empty(rect2d_t r) { return r.w <= 0 || r.h <= 0; }

static inline bool rect2d_contains(rect2d_t r, vec2_t p) {
    return p.x >= r.x && p.x < r.x + r.w
        && p.y >= r.y && p.y < r.y + r.h;
}

/* Overlap of two rectangles, empty (w or h <= 0) when they miss. */
static inline rect2d_t rect2d_intersect(rect2d_t r, rect2d_t s) {
    int x0 = r.x > s.x ? r.x : s.x;
    int y0 = r.y > s.y ? r.y : s.y;
    int x1 = (r.x + r.w) < (s.x + s.w) ? (r.x + r.w) : (s.x + s.w);
    int y1 = (r.y + r.h) < (s.y + s.h) ? (r.y + r.h) : (s.y + s.h);
    return rect2d(x0, y0, x1 - x0, y1 - y0);
}

/* Tight bounding box of a triangle. Right and bottom edges are exclusive
   so the result composes with rect2d_intersect for clipping. */
static inline rect2d_t tri2d_bounds(tri2d_t t) {
    int minx = t.a.x < t.b.x ? (t.a.x < t.c.x ? t.a.x : t.c.x) : (t.b.x < t.c.x ? t.b.x : t.c.x);
    int maxx = t.a.x > t.b.x ? (t.a.x > t.c.x ? t.a.x : t.c.x) : (t.b.x > t.c.x ? t.b.x : t.c.x);
    int miny = t.a.y < t.b.y ? (t.a.y < t.c.y ? t.a.y : t.c.y) : (t.b.y < t.c.y ? t.b.y : t.c.y);
    int maxy = t.a.y > t.b.y ? (t.a.y > t.c.y ? t.a.y : t.c.y) : (t.b.y > t.c.y ? t.b.y : t.c.y);
    return rect2d(minx, miny, maxx - minx + 1, maxy - miny + 1);
}

#endif /* GEOM_H */
