# Phase 7: Textures

## Goal

Load images, map onto triangles with perspective-correct interpolation.

## Files

- `src/util/image.h/.c` — new (BMP/TGA loader)
- `src/render/texture.h/.c` — new (texture struct, sampling)
- `src/scene/mesh.h/.c` — modify (add UV coordinates)
- `src/render/rasterizer.c` — modify (perspective-correct UV interpolation)

## Image Loading

**BMP**: 14-byte file header, 40-byte DIB header. Handle 24-bit (BGR) and 32-bit (BGRA). Rows stored bottom-up, padded to 4-byte boundaries.

**TGA**: 18-byte header. Uncompressed and RLE-compressed. 24-bit and 32-bit.

## Texture Sampling

```c
uint32_t texture_sample(const texture_t *tex, float u, float v) {
    u = u - floorf(u);  /* wrap */
    v = v - floorf(v);
    int x = (int)(u * (tex->width - 1));
    int y = (int)(v * (tex->height - 1));
    return tex->pixels[y * tex->width + x];
}
```

Nearest-neighbor. Bilinear filtering optional.

## Perspective-Correct Interpolation

Naive barycentric interpolation of UVs produces incorrect results due to perspective divide. Fix:

1. For each vertex `i`, compute `1/w_i` (clip-space W before perspective divide)
2. Interpolate `u/w`, `v/w`, `1/w` across the triangle (not `u`, `v` directly)
3. At each pixel: `u = (u/w) / (1/w)`, `v = (v/w) / (1/w)`

This is critical — without it, textures visibly swim/warp on oblique triangles.

## Verification

Textured cube with checkerboard or crate texture. No warping or swimming when rotating. Compare against affine (incorrect) version to see the difference.
