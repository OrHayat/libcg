# Phase 1: Window + Framebuffer

**Status: built.** This doc describes what exists; it has been rewritten
since the original plan, which the implementation outgrew (see
[Divergence from the original plan](#divergence-from-the-original-plan)).

## Goal

Open a native macOS window, hand the app a pixel buffer, deliver input.
Everything else builds on this.

## Files

```
src/main.c                    mode registration + window config
src/app/app.h/.c              app shell: mode list, global keys, color overlay
src/app/mode.h                the app_mode_t interface every mode implements
src/app/color_input.h/.c      '#'-triggered hex color entry
src/platform/platform.h       platform API (window, events, framebuffer, displays)
src/platform/platform_macos.m Cocoa implementation
src/render/framebuffer.h/.c   clear / set_pixel / fill_rect
src/render/color.h            hex parsing, premultiply, source-over blend
src/debug/display_info.h/.c   stdout dumps of the display-enumeration API
src/util/common.h             u8/u32/f32 typedefs, RGB/RGBA macros
```

## Architecture

Three layers, bottom-up:

**Platform** owns the event loop. The app fills in a `platform_app_desc_t`
with callbacks and window config, calls `platform_run(desc)`, and gets
called back — sokol/SDL3 style. There is no `platform_poll_events`.

**App shell** (`app.h`) sits on top: it holds the mode list and the active
index, consumes keys that mean the same thing everywhere, and runs the
`#`-hex color overlay above whichever mode is active.

**Modes** are self-contained interactive scenes implementing `app_mode_t`.
Exactly one is active. A mode owns its state behind `m->state` (allocated
in `init`, freed in `cleanup`) — no file-scope mutable state, so two
instances of a mode could coexist. Two exist today: `pattern` and `paint`
(see [paint.md](paint.md)).

`main()` is 20 lines: register the modes, describe the window, call
`app_run`.

## Platform API

```c
int  platform_run(const platform_app_desc_t *desc);   /* owns the loop */
void platform_request_quit(void);

platform_framebuffer_t *platform_get_framebuffer(void);
double   platform_now(void);            /* monotonic seconds since run start */
double   platform_dt(void);             /* seconds since previous frame */
uint64_t platform_frame_count(void);
double   platform_get_dpi_scale(void);  /* logical points -> fb pixels */

bool platform_save_dialog(const char *suggested, const char *const *exts, char *out, size_t n);
bool platform_open_dialog(const char *const *exts, char *out, size_t n);

void platform_toggle_fullscreen(void);
bool platform_is_fullscreen(void);

int      platform_get_displays(platform_display_info_t *out, int max);
int      platform_get_display_modes(uint32_t id, platform_video_mode_t *out, int max);
uint32_t platform_get_window_display_id(void);
```

Pixel format: `0xAARRGGBB`, premultiplied, top-left origin, row-major.
`fb->pixels` is valid for the duration of one `frame_cb` call only — the
buffer is handed over per frame and must not be cached across frames.

## Event Model

Every input arrives through `event_cb` exactly once, in arrival order,
before the next `frame_cb`. `platform_event_t` is a tagged union; read
only the member matching `kind`.

| Event                       | Payload                                       |
|-----------------------------|-----------------------------------------------|
| `KEY_DOWN` / `KEY_UP`       | key code, `repeat` flag, modifier bitmask     |
| `TEXT_INPUT`                | one UTF-8 codepoint                           |
| `MOUSE_DOWN` / `MOUSE_UP`   | button, position in logical points            |
| `MOUSE_MOVE`                | position + delta                              |
| `SCROLL`                    | dx, dy                                        |
| `RESIZE`                    | logical size and framebuffer pixel size       |
| `FOCUS` / `UNFOCUS` / `QUIT_REQUESTED` | none                               |

Keys are reported unshifted, with a modifier mask. Single-key bindings
must therefore test `platform_key_is_plain(e)` — otherwise `#` (Shift+3)
would also fire the binding on `3`. macOS's function and numeric-pad
flags are deliberately not mapped, so a plain arrow press reports
`mods == 0`.

Text and keys are separate streams: `#` is detected as `TEXT_INPUT`,
tool shortcuts as `KEY_DOWN`.

## macOS Implementation Notes

- `NSApplication`, `NSApplicationActivationPolicyRegular`, manual event
  pump via `nextEventMatchingMask:` — not `[NSApp run]`.
- Presentation is `CALayer.contents` with a `CGImage` per frame, not
  `drawRect:`. `drawRect:` could not keep up during live resize.
- The framebuffer is a `CGBitmapContext` in premultiplied ARGB; ownership
  transfers to the layer at present time and a fresh buffer is allocated
  for the next frame, so no frame ever draws into a buffer the compositor
  is still reading.
- Retina: with `high_dpi`, the framebuffer is allocated at
  `convertSizeToBacking:` pixels. Mouse events stay in logical points;
  `platform_get_dpi_scale()` converts.
- `windowDidChangeBackingProperties:` handles dragging between displays
  of different scale, including the same-scale case that used to flicker.
- Transparency: `setOpaque:NO` + `clearColor` when `desc->transparent`.
- File dialogs run a nested modal loop — no `frame_cb` fires while one is
  open, so `platform_now()` jumps forward across the call. Modes should
  cancel anything mid-gesture before opening one.

## Global Keys (consumed by the shell before the active mode)

| Key       | Action                                          |
|-----------|-------------------------------------------------|
| `Q`       | Quit                                            |
| `Cmd-Q`   | Quit (menu bar)                                 |
| `F`       | Toggle fullscreen                               |
| `Escape`  | Exit fullscreen (only while fullscreen)         |
| `Tab`     | Next mode                                       |
| `F1`–`F12`| Switch directly to mode N                       |
| `M`       | Toggle mouse-coordinate printing                |
| `T`       | Print framebuffer size + all display info       |
| `L`       | Print video modes for the window's display      |
| `#`       | Begin hex color input                           |

Everything else falls through to the active mode.

## Color Input (keyboard state machine)

1. `#` enters color input mode.
2. Hex digits build the string.
3. `Enter` applies `#RGB`, `#RRGGBB` or `#RRGGBBAA` via `mode->set_color`.
4. `Backspace` deletes, `Escape` cancels.

While active the overlay eats *every* event, global hotkeys included —
`f` is a hex digit, so letting `F`-fullscreen run would toggle the window
mid-`#ff0000`. Invalid input prints an error. The mode receives straight
(unpremultiplied) ARGB and premultiplies at use.

## Alpha Handling

Framebuffer contents are premultiplied. Colors typed by the user are
straight alpha until `color_premultiply`.

- No checkerboard (default): transparent colors show the desktop through
  the window.
- `B` in pattern mode: blended over a gray/white checkerboard, window
  stays opaque.

## Divergence from the Original Plan

The original doc specified "Approach B: game calls platform" with
`platform_poll_events` / `platform_present`, and a `main.c` that owned
the loop, the input handling and the debug patterns. What shipped:

- The loop inverted — `platform_run` drives callbacks (#20, #19).
- Polled input became per-event delivery through `event_cb`.
- Presentation moved to `CALayer.contents` (#21), then to
  buffer-ownership transfer (#23).
- `main.c` split into an app shell plus pluggable modes (#30), after an
  intermediate step that removed the globals (#28).
- The API grew display enumeration (#16), modifier reporting (#40), and
  native file dialogs (#39).

## Verification

- Window opens at 1280x720, centered, resizes without flicker.
- Patterns `1`–`4` work; `#FF0000` turns the window red.
- `#FF000080` shows the desktop through a red tint; with `B`, red over
  the checkerboard.
- `T` reports doubled resolution on retina (e.g. 2560x1440).
- Fullscreen toggles; `Q` and `Cmd-Q` both quit cleanly.
- Dragging between a retina and a non-retina display reallocates cleanly.
