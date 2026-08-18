# Paint Mode

**Status: built.** Off-roadmap — this is not one of the numbered phases.
It exists as an exercise for `draw2d` and the platform layer, and it is
what drove modifier reporting (#40), native file dialogs (#39) and the
BMP loader (#42) into the platform and util layers. Nothing in phases
3–9 depends on it.

## Model

MS Paint, roughly. A fixed-size pixel canvas, letterboxed inside the
window with a 1px border. The canvas is allocated at the startup
framebuffer size (retina-aware) and survives every window resize — the
window is only a viewport onto it. Opening a file re-sizes the canvas to
the image rather than scaling the image into the canvas.

## Controls

| Key       | Action                                                       |
|-----------|--------------------------------------------------------------|
| `1`       | Pencil — 1px                                                 |
| `2`       | Brush — `brush_size` square                                  |
| `3`       | Eraser — `brush_size` square of white                        |
| `4`       | Line — press, drag with live preview, release to commit      |
| `5`       | Triangle, filled — three clicks                              |
| `6`       | Triangle, outline — three clicks                             |
| `[` / `]` | Width 1..32                                                  |
| `C`       | Clear canvas                                                 |
| `S` / `O` | Save / open BMP through the native dialog                    |
| `Esc`     | Cancel a shape in progress                                   |
| `#`       | (shell) set color — its alpha is opacity                     |

`[` and `]` walk one continuous 1..32 ramp: width 1 *is* the pencil,
2 and up is the brush, so stepping down past 2 selects the pencil rather
than dead-ending. Shape corners may be placed in the letterbox; the
rasterizer clips on commit.

## Stroke Coverage Mask

The one design decision worth knowing. Tools do not draw onto the canvas
— they mark coverage in a canvas-sized `u8` mask, and the whole mask is
composited in a single blend when the stroke finishes.

Compositing per stamp instead applies the color once per overlapping
stamp. A 5px brush overlaps itself about 5 times per pixel, so a
25%-alpha stroke lands at 76%, and the result depends on how fast the
mouse moved. One composite per stroke means the alpha you typed is the
opacity you get. (#41, #43)

The in-progress stroke is previewed by blitting the same mask with the
same single blend, so the preview matches the committed result exactly.

The mask is binary today (0 or 255); `u8` leaves room for antialiased
coverage. An inclusive dirty rect bounds every clear, composite and
redraw, so a small stroke never costs a full-canvas sweep.

## Alpha and Files

The canvas is opaque paper: loaded images are flattened onto white, and
`image_save_bmp` writes 24-bit. BMP's 32-bit form declares its fourth
byte "reserved" and decoders disagree about whether it means alpha, so
24-bit is the variant that round-trips everywhere. The *loader* is more
permissive — 16/24/32-bit, `BI_RGB` or `BI_BITFIELDS`, with channel
positions read from the file's own masks rather than an assumed byte
order, because GIMP and Photoshop both write 32-bit BMPs as
`BI_BITFIELDS` (#42).

File dialogs run a nested modal loop, so the mode cancels anything
mid-gesture before opening one: a freehand stroke in progress is real
work and gets committed, an unfinished rubber-band shape is dropped.
