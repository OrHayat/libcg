# libcg docs

A software rasterizer built from scratch in C: window and framebuffer,
2D primitives, then a 3D pipeline.

Phases 1–2 are built, and their docs describe what exists. Phases 3–9 are
plans and describe what is intended — expect them to be rewritten as they
land, the way phase 1 was.

| Doc                    | Scope                                                   | Status  |
|------------------------|---------------------------------------------------------|---------|
| [phase1](phase1.md)    | Window, framebuffer, events, app shell + modes           | built   |
| [phase2](phase2.md)    | Lines and triangles in screen space                      | built   |
| [paint](paint.md)      | Paint mode — off-roadmap exercise for the above          | built   |
| [phase3](phase3.md)    | Math library: vec2/3/4, mat4, transforms                 | next    |
| [phase4](phase4.md)    | 3D wireframe: MVP pipeline, camera                       | planned |
| [phase5](phase5.md)    | Solid rendering: z-buffer, back-face culling             | planned |
| [phase6](phase6.md)    | Lighting: flat / Gouraud / Phong, gamma                  | planned |
| [phase7](phase7.md)    | Textures: perspective-correct UV                         | partial |
| [phase8](phase8.md)    | OBJ model loading                                        | planned |
| [phase9](phase9.md)    | Win32 / X11 platform backends                            | planned |

Phase 7 is marked partial because its BMP loader landed early, pulled in
by paint mode's save/open.

## Layout

```
src/main.c        mode registration + window config
src/app/          app shell: mode list, global keys, color-input overlay
src/modes/        one file per interactive scene (pattern, paint)
src/platform/     platform API + macOS Cocoa backend
src/render/       framebuffer, color, draw2d
src/util/         typedefs, BMP read/write
src/debug/        stdout dumps of display info
```

## Build

```
make          # -> build/libcg
make run
make compdb   # compile_commands.json for clangd (needs `bear`)
```
