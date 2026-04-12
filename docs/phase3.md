# Phase 3: Math Library

## Goal

Vector and matrix types, linear algebra, projection and viewport transforms.

## Files

- `src/math/vec.h` — vec2, vec3, vec4 (inline functions)
- `src/math/mat4.h/.c` — 4x4 matrix operations
- `src/math/transform.h/.c` — model/view/projection builders

## Types

```c
typedef struct { float x, y; } vec2_t;
typedef struct { float x, y, z; } vec3_t;
typedef struct { float x, y, z, w; } vec4_t;
```

Structs with named fields, not arrays. `static inline` operations in `vec.h`: add, sub, mul (scalar), dot, cross (vec3), normalize, length.

## Matrix Convention

`mat4_t` is `float m[4][4]`, **column-major**: `m[col][row]`. `m[3][0]` is the X translation. This matches OpenGL convention and most reference material.

## Transform Builders

- `mat4_identity()`
- `mat4_translate(vec3_t t)`
- `mat4_rotate_x/y/z(float radians)`
- `mat4_scale(vec3_t s)`
- `mat4_look_at(vec3_t eye, vec3_t target, vec3_t up)`
- `mat4_perspective(float fov_radians, float aspect, float near, float far)`
- `mat4_mul(mat4_t a, mat4_t b)`
- `mat4_mul_vec4(mat4_t m, vec4_t v)`

## Viewport Transform

Maps NDC `[-1, 1]` to screen pixels. Flips Y (NDC Y-up → screen Y-down):

```c
screen_x = (ndc.x + 1) * 0.5 * width
screen_y = (1 - ndc.y) * 0.5 * height
```

## Verification

No visual change — library phase. Print matrix/vector results to stdout and verify against known values.
