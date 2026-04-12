# Phase 6: Lighting

## Goal

Flat, Gouraud, and Phong shading. Gamma correction.

## Files

- `src/render/lighting.h/.c` — new
- `src/scene/mesh.h/.c` — modify (add normals)
- `src/render/rasterizer.c` — modify (interpolate normals/colors)
- `src/main.c` — modify

## Gamma Correction

sRGB ↔ linear lookup tables (256 entries each). All lighting math in linear space, convert to sRGB before writing to framebuffer. Without this, dark areas look crushed.

## Light

```c
typedef struct {
    vec3_t direction;   /* normalized */
    vec3_t color;       /* RGB in [0, 1] */
    float  ambient;
} light_t;
```

## Normals

- **Face normals**: cross product of two triangle edges, normalized
- **Vertex normals**: average of face normals sharing each vertex (for smooth shading)

## Three Shading Modes (toggle with 1/2/3)

1. **Flat**: one lighting calc per triangle, all pixels same color
2. **Gouraud**: light at each vertex, interpolate color across triangle
3. **Phong**: interpolate normal across triangle, light per pixel

## Lighting Formula

- Diffuse: `max(0, dot(N, L))`
- Specular: `pow(max(0, dot(R, V)), shininess)`
- Final: `ambient + diffuse + specular`, clamped to [0, 1]

## Verification

Lit rotating cube or sphere. Press 1/2/3 to toggle modes. Flat shows facets, Gouraud is smooth, Phong has correct specular highlights.
