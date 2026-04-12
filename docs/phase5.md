# Phase 5: Solid Rendering

## Goal

Fill triangles with solid color, z-buffer for depth, back-face culling.

## Files

- `src/render/rasterizer.h/.c` — new
- `src/render/framebuffer.h/.c` — modify (add depth buffer)
- `src/main.c` — modify

## Z-Buffer

`float` array, same size as framebuffer, cleared to `1.0` per frame. When rasterizing a pixel, compare interpolated depth against z-buffer. If closer (smaller), write pixel and update z-buffer.

Depth is interpolated using barycentric coordinates from the edge functions (Phase 2).

## Back-Face Culling

Compute signed area in screen space before rasterizing:

```c
float area = (v1.x - v0.x) * (v2.y - v0.y) - (v2.x - v0.x) * (v1.y - v0.y);
if (area <= 0) return;  /* back-facing, skip */
```

Assumes counter-clockwise winding = front-facing.

## Verification

Solid cube with each face in a different color. Faces correctly occlude each other. Back faces invisible.
