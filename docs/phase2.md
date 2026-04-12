# Phase 2: 2D Primitives

## Goal

Draw pixels, lines, and triangles directly into the framebuffer in 2D screen space.

## Files

- `src/render/draw2d.h/.c` — new
- `src/render/framebuffer.h/.c` — modify (add `framebuffer_set_pixel`)
- `src/main.c` — modify (draw test primitives)

## Algorithms

### Bresenham Line Drawing

Integer-only line algorithm. Handles all octants (all slopes, positive and negative). Steps one pixel along the major axis per iteration, accumulates error for the minor axis.

### Triangle Rasterization — Edge Functions (Barycentric)

For each pixel in the triangle's bounding box, compute three edge function values:

```
E01(P) = (v1.x - v0.x) * (P.y - v0.y) - (v1.y - v0.y) * (P.x - v0.x)
```

If all three have the same sign, the pixel is inside. The edge function values are the unnormalized barycentric coordinates — this generalizes directly to depth, color, and UV interpolation in later phases.

### Wireframe Triangles

Three `draw_line` calls per triangle.

## Verification

- Lines at various angles (starburst pattern)
- Wireframe triangle in white
- Filled red triangle
- Filled green triangle overlapping red (verify edge correctness)
