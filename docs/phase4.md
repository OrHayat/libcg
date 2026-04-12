# Phase 4: 3D Wireframe

## Goal

Transform 3D vertices through Model-View-Projection pipeline, draw wireframe on screen.

## Files

- `src/scene/mesh.h/.c` — new (mesh data structure, hardcoded cube)
- `src/scene/camera.h/.c` — new (camera state, view matrix, fly controls)
- `src/main.c` — modify (MVP pipeline, render loop)

## Mesh Data Structure

```c
typedef struct {
    vec3_t *vertices;
    int     vertex_count;
    int    *indices;          /* 3 per triangle */
    int     index_count;
} mesh_t;
```

Cube: 8 vertices, 12 triangles, 36 indices.

## MVP Pipeline (per vertex)

1. Multiply by Model matrix (rotate cube over time)
2. Multiply by View matrix (from camera)
3. Multiply by Projection matrix (perspective)
4. Perspective divide: `x /= w; y /= w; z /= w`
5. Clip check: reject if outside `[-1, 1]` or `w <= 0`
6. Viewport transform: NDC → screen pixels
7. Draw edges with `draw_line`

## Camera

Position + pitch/yaw. WASD moves in camera's local forward/right plane. Arrow keys adjust pitch/yaw. View matrix via `mat4_look_at(eye, eye + forward, up)`.

## Verification

Rotating wireframe cube in perspective. Arrow keys / WASD move the camera.
