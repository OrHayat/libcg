# UI Layer

**Status: built.** Off-roadmap, like [paint mode](paint.md) — nothing in
phases 3–9 depends on it. It exists so paint mode is usable with the
mouse instead of memorised keys, and it gives the later phases something
to draw debug overlays with, since the 3D pipeline has no text of its own.

## Model

**Immediate-mode widgets, retained panels.** The split Dear ImGui uses,
and the right one here.

A widget is a function call, not an object. It draws itself and returns
what happened:

```c
if (ui_button(&ui, "Pencil", st->tool == TOOL_PENCIL))
    paint_exec(st, CMD_TOOL_PENCIL);
```

Nothing is created, registered or freed, and there is no callback to keep
in sync with the state it edits. The button's highlight is an argument,
read fresh from `paint_state_t` every frame — which is why picking a tool
with the keyboard lights the button up with no plumbing at all. A
retained widget holding its own "am I selected" flag would need telling,
and the desync between widget and model is the classic bug that design
invites.

Panels are the exception and *are* retained: a closed panel has to reopen
where the user left it, so `open`, `x` and `y` outlive the frame. The
caller owns the struct — there is no registry, and the mode holding them
keeps them in its own state like everything else.

## Identity and Capture

With no objects, a widget is identified by `(panel slot, call order within
the panel)`. Two ids persist between frames:

- `hot` — under the cursor
- `active` — pressed on, held until release

A click is "released while `active` is mine and the cursor is still over
me". `active` is also the mouse capture: it is what keeps a slider drag
tracking after the cursor slides off the slider, and it means a widget
must not vanish mid-drag. A panel that needs to hide a widget should draw
it disabled instead, or the ids after it shift and a drag in progress
jumps to a different control.

## Event Routing

`ui_event` sees every event before the mode does and returns whether it
consumed it. Without that, a click on a button would also land on the
canvas underneath and paint a dot.

Hit testing at event time uses the *previous* frame's rects, because the
panel is drawn during `frame()`, which has not run yet. A panel moved on
the previous frame is one frame stale. This is the standard
immediate-mode trade and is invisible in practice.

A press that starts on the canvas keeps its capture: `ui_event` only
claims presses that begin on a panel, so dragging a stroke out over a
panel keeps painting — onto the canvas beneath it.

## Auto-sizing Lags One Frame

A panel's height is whatever its widgets added up to, but the chrome is
drawn before those widgets are issued, so `ui_panel_begin` uses last
frame's height. The only visible consequence is that a panel opened this
frame draws its background one frame late. Dropdown menus dodge this by
taking their row count as an argument instead.

This is the honest cost of single-pass layout: anything that needs to
size itself to content it has not seen yet is either a frame behind or
told the answer up front.

## No Text Before This

The codebase had no glyph rendering at all — `draw2d` has lines and
triangles. `src/ui/font` adds an 8×8 bitmap font: a 5×7 face plus a row
for the descenders of `g j p q y`, one byte per row, MSB leftmost, so a
row reads left-to-right as its own bit pattern and `0xF8` is a solid 5px
bar. Printable ASCII only; anything else draws a hollow box, so a stray
byte shows up on screen rather than rendering as blank space.

Scaling is an integer pixel multiplier, deliberately unfiltered — a
doubled bitmap font should stay crisp rather than turn to mush. The whole
sheet is on pattern mode's key `6`.

## Commands, Not Callbacks

Buttons do not own behavior. Every action paint mode can perform is a
named command in one table, and both the keyboard and the toolbar
dispatch through `paint_exec`:

```c
static const paint_binding_t PAINT_COMMANDS[] = {
    { CMD_TOOL_PENCIL, "Pencil", GROUP_TOOL, PLATFORM_KEY_1, 0 },
    /* ... */
};
```

Bindings are data, so both input paths are the same loop over the same
array, and the panel a button lands in is a column rather than code. The
invariant that makes this worth doing: **every command is reachable from
the UI, and the keyboard is only an accelerator.** A command that were
keyboard-only would simply be gone on a platform without one.

The `mods` column is `0` on every row today. It stays because exact-match
on `mods == 0` *is* the unmodified-key rule — it is what stops `Shift+3`
(`#`, the shell's color input) from also selecting the eraser. Matching a
subset instead would make `Ctrl+Shift+S` fire the binding for `Ctrl+S`.

Anything platform-shaped stays behind `src/platform/`. A `Cmd`-vs-`Ctrl`
`#ifdef` in the command table would have been the first one outside that
directory; the accelerator belongs in the platform layer as a semantic
modifier, since only the backend knows which physical key plays that role.

## Layout

```
src/ui/font.h/.c   8x8 bitmap font, glyph table + blended scaled draw
src/ui/ui.h/.c     context, panels, menu bar, button / swatch / slider
```

Panels and the menu bar are drawn by the mode that owns them, in its
`frame()`. The app shell needed no changes at all — the UI is a
mode-level concern, not a shell service, so each mode instantiates its
own `ui_t` and its own panels.

## Verification

Paint mode, all four panels. The menu bar's `Panels` menu toggles each
one; a panel closed by its title-bar `x` comes back from there, in the
place it was left. Pattern mode key `6` is the font sheet.
