# Phase 1: Window + Framebuffer

## Goal

Open a native macOS window, allocate a pixel buffer, display debug patterns, handle input. This is the foundation everything else builds on.

## Files

- `Makefile` — build system
- `src/util/common.h` — typedefs (u8, u32, f32...), RGB/RGBA macros
- `src/platform/platform.h` — platform abstraction interface
- `src/platform/platform_macos.m` — macOS Cocoa implementation
- `src/render/framebuffer.h/.c` — pixel buffer utilities
- `src/main.c` — main loop, input handling, debug patterns

## Architecture

**Approach B: Game calls Platform.** `main.c` owns the main loop and calls `platform_*` functions. The platform layer is a library.

Platform internal state is organized as nested structs grouped by responsibility:

```c
typedef struct { NSApplication *app; } macos_app_t;
typedef struct { NSWindow *window; NSView *view; } macos_window_t;
typedef struct { CGContextRef context; uint32_t *pixels; int w, h; } macos_framebuffer_t;

typedef struct {
    macos_app_t         app;
    macos_window_t      window;
    macos_framebuffer_t framebuffer;
} platform_state_t;

static platform_state_t state;
```

## Platform Abstraction

```c
bool platform_init(int w, int h, const char *title);
void platform_shutdown(void);
void platform_poll_events(platform_input_t *input);
void platform_present(void);
platform_framebuffer_t *platform_get_framebuffer(void);
double platform_get_time(void);
void platform_toggle_fullscreen(void);
bool platform_is_fullscreen(void);
```

Pixel format: `0xAARRGGBB` (ARGB8888). Top-left origin, row-major.

## macOS Cocoa Details

- `NSApplication` with `NSApplicationActivationPolicyRegular`
- Custom `NSView` subclass, override `drawRect:` to blit pixel buffer
- Pixel buffer via `CGBitmapContextCreate` with premultiplied alpha
- Manual event pump via `nextEventMatchingMask:` (not `[NSApp run]`)
- `@autoreleasepool` around each frame's event polling
- Window: 1280x720, centered, resizable, fullscreen-capable
- Retina: framebuffer allocated at physical resolution via `convertSizeToBacking:`
- Window transparency: `setOpaque:NO`, `setBackgroundColor:clearColor`

## Keyboard Controls

| Key      | Action                                         |
|----------|------------------------------------------------|
| `1`      | Solid color fill                               |
| `2`      | Gradient (horizontal)                          |
| `3`      | Color cycle (animated)                         |
| `4`      | Random noise                                   |
| `B`      | Toggle alpha background: opaque / checkerboard |
| `M`      | Toggle mouse coordinate printing (debug)       |
| `T`      | Print framebuffer size to stdout               |
| `F`      | Toggle fullscreen                              |
| `Q`      | Quit                                           |
| `Cmd-Q`  | Quit (via menu bar)                            |
| `Escape` | Exit fullscreen                                |
| `#`      | Enter color input mode                         |

## Color Input (keyboard state machine)

1. Press `#` — enters color input mode
2. Type hex digits (0-9, a-f) — builds color string
3. `Enter` — apply color (#RGB, #RRGGBB, or #RRGGBBAA)
4. `Backspace` — delete last character
5. `Escape` — cancel

Invalid input prints error to stdout. All state changes logged to stdout.

## Alpha Handling

- Colors with alpha < 0xFF are premultiplied before writing to framebuffer
- Without checkerboard (default): transparent colors show desktop through window
- With checkerboard (B key): transparent colors blended over gray/white checkerboard, window stays opaque

## Commit Breakdown

```
1.  open empty white window, close via X button
2.  add menu bar, Cmd-Q to quit
3.  add pixel buffer, fill with static color
4.  add keyboard input, Q key to quit
5.  add mouse input
6.  add resizable window
7.  add retina/HiDPI
8.  add fullscreen toggle
9.  add framebuffer utilities (framebuffer.h/.c)
10. add debug patterns (1-4 keys)
11. add B key background toggle + window transparency
12. add # color input state machine
13. add T key framebuffer info
```

## Verification

- Window opens at 1280x720, centered
- Keyboard patterns 1-4 work
- `#FF0000` via keyboard turns window red
- `#FF000080` with B toggled shows red over checkerboard
- `#FF000080` without B shows desktop through red tint
- Resize reallocates framebuffer
- T key shows doubled resolution on retina (e.g. 2560x1440)
- Fullscreen toggles correctly
- Cmd-Q and Q both quit cleanly
