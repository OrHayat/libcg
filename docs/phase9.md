# Phase 9: Additional Platforms

## Goal

Add Win32 and X11 (and optionally Wayland) platform backends. Same renderer code, different platform layer.

## Files

- `src/platform/platform_win32.c` — new
- `src/platform/platform_x11.c` — new
- `src/platform/platform_wayland.c` — new (optional)
- `Makefile` — modify (platform detection)

## Win32

- `RegisterClassExW` + `CreateWindowExW` for window
- `CreateDIBSection` for pixel buffer (returns pointer to pixel memory)
- `BitBlt` to blit DIB to window DC
- `PeekMessageW` event loop (non-blocking)
- `QueryPerformanceCounter` / `QueryPerformanceFrequency` for time
- `SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)` for multi-monitor DPI
- Handle `WM_DPICHANGED` for monitor transitions

## X11

- `XOpenDisplay`, `XCreateSimpleWindow`, `XMapWindow`
- `XCreateImage` with `ZPixmap` format pointing at pixel array
- `XPutImage` to blit
- `XPending` + `XNextEvent` for events (non-blocking check)
- `clock_gettime(CLOCK_MONOTONIC)` for time
- Optional: MIT-SHM extension (`XShmCreateImage`) for zero-copy

## Wayland (optional)

- `wl_display_connect`, registry listener to discover interfaces
- `wl_compositor` → `wl_surface` → `xdg_surface` → `xdg_toplevel` (3 objects for 1 window)
- Shared memory pixel buffer via `shm_open` + `mmap` + `wl_shm_create_pool`
- `libdecor` for window decorations (title bar, close button)
- `libxkbcommon` for keyboard handling

## Makefile Platform Detection

```makefile
UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
    PLATFORM_SRC = src/platform/platform_macos.m
    LDFLAGS      = -framework Cocoa -framework QuartzCore
else ifeq ($(UNAME_S),Linux)
    PLATFORM_SRC = src/platform/platform_x11.c
    LDFLAGS      = -lX11 -lm
else ifeq ($(OS),Windows_NT)
    PLATFORM_SRC = src/platform/platform_win32.c
    LDFLAGS      = -lgdi32 -luser32
endif
```

## Testing

- **macOS**: build and run directly
- **Windows**: build and run on Windows PC
- **Linux/X11**: build and run via WSL2 on Windows PC

## Verification

Same renderer output on all platforms. Native window, correct input handling, correct pixel display.
