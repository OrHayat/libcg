# Phase 2: 2D Primitives

**Status: built.**

## Goal

Draw pixels, lines, and triangles directly into the framebuffer in 2D
screen space.

## Files

```
src/render/draw2d.h/.c        lines, wireframe and filled triangles
src/render/framebuffer.h/.c   framebuffer_set_pixel, fill_rect
src/modes/pattern.c           key 5: the primitives showcase
```

## API

```c
typedef void (*draw2d_pixel_fn)(int x, int y, void *user_data);

void draw2d_walk_line(int x0, int y0, int x1, int y1, draw2d_pixel_fn fn, void *ud);
void draw2d_line(fb, x0, y0, x1, y1, color);
void draw2d_triangle_wire(fb, x0, y0, x1, y1, x2, y2, color);
void draw2d_triangle_fill(fb, x0, y0, x1, y1, x2, y2, color);
void draw2d_triangle_fill_blend(fb, x0, y0, x1, y1, x2, y2, premul_color);
```

The Bresenham walk is exposed as a callback rather than only as a
framebuffer write, because the caller decides what a visited pixel means.
`draw2d_line` writes a pixel; paint mode's line tool stamps a brush
footprint and marks a coverage mask with the same walk.

Coordinates are integer, top-left origin, endpoints inclusive. Everything
clips per pixel to the framebuffer — callers may pass coordinates off
screen (paint mode's shape tools do).

## Algorithms

### Bresenham Line Drawing

Integer-only, all octants. Steps one pixel along the major axis per
iteration and accumulates error for the minor axis.

### Triangle Rasterization — Edge Functions (Barycentric)

For each pixel in the triangle's bounding box, compute three edge
function values:

```
E01(P) = (v1.x - v0.x) * (P.y - v0.y) - (v1.y - v0.y) * (P.x - v0.x)
```

Same sign on all three means the pixel is inside. These values are the
unnormalized barycentric weights that phases 5–7 will interpolate depth,
color and UV with.

Two properties beyond the original plan:

- **Winding-agnostic.** Vertices are reordered internally to positive
  area, so callers need not care about winding. (Back-face culling in
  phase 5 wants the *opposite* — it must reject by winding before
  rasterizing, so it will test the signed area itself.)
- **Top-left fill rule.** Two triangles sharing an edge cover the shared
  pixels exactly once: no background gap, and no double-blend. Without
  this, the blended variant would darken every shared seam.

### Blended Fill

`draw2d_triangle_fill_blend` is source-over instead of replace; both the
source color and the destination must be premultiplied. It exists because
paint mode draws translucent shapes.

## Verification

Pattern mode, key `5` — one primitive per quadrant, sized off the
framebuffer so it holds at any window size:

| Quadrant     | Shows                                                            |
|--------------|------------------------------------------------------------------|
| Top-left     | 24-spoke starburst — every octant of the line walker             |
| Top-right    | Wireframe triangle                                               |
| Bottom-left  | Two overlapping filled triangles — edge coverage under overwrite |
| Bottom-right | Two triangles sharing a diagonal — dark pixels on the seam would be a fill-rule bug |
