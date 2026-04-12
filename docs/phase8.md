# Phase 8: Model Loading

## Goal

Parse OBJ files, load textures, render complete 3D models.

## Files

- `src/scene/obj_loader.h/.c` — new
- `src/scene/mesh.h/.c` — modify (support multiple meshes/materials)
- `src/main.c` — modify

## OBJ Format

Line-by-line text parsing:

- `v x y z` — vertex position
- `vt u v` — texture coordinate
- `vn x y z` — vertex normal
- `f v/vt/vn v/vt/vn v/vt/vn` — face (1-based indices)
- `mtllib filename.mtl` — material library
- `usemtl name` — switch material

Also handle `v//vn` (no texture) and `v` (position only).

## Face Index Handling

OBJ allows separate indices for position, UV, and normal. Each unique `(pos_idx, uv_idx, normal_idx)` tuple becomes a distinct vertex. Deduplicate with linear scan or hash map.

## Quads

Many OBJ files have quad faces (4 vertices). Split into two triangles: `(v0, v1, v2)` and `(v0, v2, v3)`.

## MTL Parsing (optional)

- `map_Kd texture.bmp` — diffuse texture path
- `Ka`, `Kd`, `Ks`, `Ns` — material colors and shininess

## Verification

Textured, lit 3D model loaded from OBJ file, rotating on screen with camera orbit.
