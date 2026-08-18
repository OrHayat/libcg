#include "modes/paint.h"
#include "render/color.h"
#include "render/draw2d.h"
#include "render/framebuffer.h"
#include "util/image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    TOOL_PENCIL = 0,    /* 1px dot at cursor, current color */
    TOOL_BRUSH,         /* brush_size × brush_size square, current color */
    TOOL_ERASER,        /* brush_size × brush_size square, white */
    TOOL_LINE,          /* press–drag–release straight line, brush_size wide */
    TOOL_TRIANGLE,      /* click 3 corners, filled with current color */
    TOOL_TRIANGLE_WIRE, /* click 3 corners, outline brush_size wide */
} paint_tool_t;

static bool tool_is_triangle(paint_tool_t t) {
    return t == TOOL_TRIANGLE || t == TOOL_TRIANGLE_WIRE;
}

typedef struct {
    /* fixed-size canvas that survives window resizes */
    pcolor_t *canvas;
    int  canvas_w, canvas_h;

    paint_tool_t tool;              /* default TOOL_PENCIL */
    int          brush_size;        /* 1..32, default 5 */
    color_t      color;             /* straight, from the shell's # input */
    bool         painting;          /* mouse held during a stroke */
    int          last_cx, last_cy;  /* prev stroke point in canvas coords; -1 = none */

    /* line tool: rubber-band from anchor to cursor while the button is
       held; committed to the canvas on release. Coords may lie outside
       the canvas — stamping clips per pixel. */
    bool         line_active;
    int          line_x0, line_y0;
    int          line_x1, line_y1;

    /* triangle tool: click-click-click. tri_n counts committed corners
       (0..2); slot [tri_n] tracks the cursor so the rubber-band preview
       always has a live third point. The 3rd click commits and resets. */
    int          tri_n;
    int          tri_x[3], tri_y[3];

    /* Stroke coverage mask, canvas-sized. Tools mark coverage here rather
       than drawing onto the canvas, and the whole mask is composited in one
       pass when the stroke finishes. Compositing per stamp instead would
       apply the color once per overlapping stamp — a 5px brush overlaps
       itself 5 times per pixel, turning a 25%-alpha stroke into 76%, and
       making the result depend on how fast the mouse moved.

       0 = untouched, 255 = covered. Binary today; u8 leaves room for
       antialiased coverage later. The dirty rect is inclusive and bounds
       every clear, composite and redraw, so a small stroke never costs a
       full-canvas sweep. Empty is dirty_x1 < dirty_x0. */
    u8  *stroke;
    int  dirty_x0, dirty_y0, dirty_x1, dirty_y1;
} paint_state_t;

#define CANVAS_BG PCOLOR_RGB(0xFF, 0xFF, 0xFF)

/* Letterbox offset of the canvas inside the framebuffer. Single source
   of truth — render, hit-testing and the line preview all use it, so
   the cursor always lands exactly on the painted pixel. */
static void canvas_offset(const platform_framebuffer_t *fb, const paint_state_t *st,
                          int *off_x, int *off_y) {
    *off_x = (fb->width  - st->canvas_w) / 2;
    *off_y = (fb->height - st->canvas_h) / 2;
}

/* Fill a size×size square anchored on (cx,cy) into any pixel grid, clipped
   to [0,w)×[0,h). Width is exactly `size`: a 4 really is 4px across, sitting
   half a pixel off centre. Deriving a radius as size/2 instead would make
   every even size identical to the odd one below it, so half the [ / ]
   presses would change nothing on screen.

   Marks coverage only — the color is applied later, once, by
   stroke_composite. */
static void stamp_square(paint_state_t *st, int cx, int cy, int size) {
    int x0 = cx - (size - 1) / 2, x1 = x0 + size - 1;
    int y0 = cy - (size - 1) / 2, y1 = y0 + size - 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= st->canvas_w) x1 = st->canvas_w - 1;
    if (y1 >= st->canvas_h) y1 = st->canvas_h - 1;
    if (x0 > x1 || y0 > y1) return;

    for (int y = y0; y <= y1; y++)
        memset(&st->stroke[(size_t)y * st->canvas_w + x0], 0xFF, (size_t)(x1 - x0 + 1));

    if (x0 < st->dirty_x0) st->dirty_x0 = x0;
    if (y0 < st->dirty_y0) st->dirty_y0 = y0;
    if (x1 > st->dirty_x1) st->dirty_x1 = x1;
    if (y1 > st->dirty_y1) st->dirty_y1 = y1;
}

static bool stroke_is_empty(const paint_state_t *st) {
    return st->dirty_x1 < st->dirty_x0 || st->dirty_y1 < st->dirty_y0;
}

/* Drop the pending stroke, clearing only what was marked. */
static void stroke_reset(paint_state_t *st) {
    if (!stroke_is_empty(st)) {
        size_t run = (size_t)(st->dirty_x1 - st->dirty_x0 + 1);
        for (int y = st->dirty_y0; y <= st->dirty_y1; y++)
            memset(&st->stroke[(size_t)y * st->canvas_w + st->dirty_x0], 0, run);
    }
    st->dirty_x0 = st->canvas_w;
    st->dirty_y0 = st->canvas_h;
    st->dirty_x1 = -1;
    st->dirty_y1 = -1;
}

/* Blend `color` into `target` wherever the mask is set. (off_x, off_y) maps
   canvas coordinates onto the target grid — zero for the canvas itself, the
   letterbox offset when previewing into the framebuffer. */
static void stroke_blit(const paint_state_t *st, pcolor_t *target, int tw, int th,
                        int off_x, int off_y, pcolor_t color) {
    if (stroke_is_empty(st)) return;
    for (int y = st->dirty_y0; y <= st->dirty_y1; y++) {
        int ty = y + off_y;
        if (ty < 0 || ty >= th) continue;
        const u8 *mask = &st->stroke[(size_t)y * st->canvas_w];
        pcolor_t *row  = &target[(size_t)ty * tw];
        for (int x = st->dirty_x0; x <= st->dirty_x1; x++) {
            if (!mask[x]) continue;
            int tx = x + off_x;
            if (tx < 0 || tx >= tw) continue;
            row[tx] = color_blend(row[tx], color);
        }
    }
}

/* ---- render: letterbox the canvas inside the framebuffer ----
   Window > canvas → gray bars; window < canvas → canvas pixels are
   clipped from view but remain in memory. No interpolation, ever.
   Invariant: this and mouse_to_canvas use the same offset formula
   so the cursor lands exactly on the painted pixel. */

static void render_canvas(platform_framebuffer_t *fb, const paint_state_t *st) {
    const pcolor_t *canvas = st->canvas;
    pcolor_t       *fbpx   = pcolor_pixels(fb->pixels);
    int canvas_w = st->canvas_w, canvas_h = st->canvas_h;

    framebuffer_clear(fb, PCOLOR_RGB(48, 48, 48));
    if (!canvas) return;

    int off_x, off_y;
    canvas_offset(fb, st, &off_x, &off_y);

    int dst_x0 = off_x < 0 ? 0 : off_x;
    int dst_y0 = off_y < 0 ? 0 : off_y;
    int dst_x1 = off_x + canvas_w;
    int dst_y1 = off_y + canvas_h;
    if (dst_x1 > fb->width)  dst_x1 = fb->width;
    if (dst_y1 > fb->height) dst_y1 = fb->height;
    if (dst_x0 >= dst_x1 || dst_y0 >= dst_y1) return;

    int src_x = dst_x0 - off_x;
    size_t row_bytes = (size_t)(dst_x1 - dst_x0) * sizeof *canvas;
    for (int y = dst_y0; y < dst_y1; y++) {
        memcpy(&fbpx[y * fb->width + dst_x0],
               &canvas[(y - off_y) * canvas_w + src_x],
               row_bytes);
    }

    /* 1px black border around the canvas. Horizontal strips run from the
       LEFT outer column (off_x - 1) through the RIGHT outer column
       (off_x + canvas_w) so the corners get covered — without that,
       the four corner pixels stay gray and the border looks chipped. */
    int top   = off_y - 1;
    int bot   = off_y + canvas_h;
    int left  = off_x - 1;
    int right = off_x + canvas_w;

    int hx0 = left  < 0           ? 0           : left;
    int hx1 = right >= fb->width  ? fb->width-1 : right;   /* inclusive */
    int vy0 = top   < 0           ? 0           : top;
    int vy1 = bot   >= fb->height ? fb->height-1: bot;     /* inclusive */

    if (top >= 0 && top < fb->height) {
        for (int x = hx0; x <= hx1; x++)
            fbpx[top * fb->width + x] = PCOLOR_RGB(0, 0, 0);
    }
    if (bot >= 0 && bot < fb->height) {
        for (int x = hx0; x <= hx1; x++)
            fbpx[bot * fb->width + x] = PCOLOR_RGB(0, 0, 0);
    }
    if (left >= 0 && left < fb->width) {
        for (int y = vy0; y <= vy1; y++)
            fbpx[y * fb->width + left] = PCOLOR_RGB(0, 0, 0);
    }
    if (right >= 0 && right < fb->width) {
        for (int y = vy0; y <= vy1; y++)
            fbpx[y * fb->width + right] = PCOLOR_RGB(0, 0, 0);
    }
}

/* ---- tools ---- */

static const char *tool_name(paint_tool_t t) {
    switch (t) {
    case TOOL_PENCIL: return "pencil";
    case TOOL_BRUSH:  return "brush";
    case TOOL_ERASER: return "eraser";
    case TOOL_LINE:   return "line";
    case TOOL_TRIANGLE:      return "triangle";
    case TOOL_TRIANGLE_WIRE: return "triangle outline";
    default:          return "?";
    }
}

/* Convert logical-point mouse coords to canvas-pixel coords. Mouse events
   arrive in logical points; the canvas is in framebuffer pixels, so scale
   by the DPI factor then subtract the letterbox offset. Always writes the
   (possibly out-of-range) coords; returns whether they're inside the
   canvas rather than in the gray bars. */
static bool mouse_to_canvas(const paint_state_t *st, int mouse_x, int mouse_y,
                            int *out_cx, int *out_cy) {
    /* Only width/height are read here — safe outside frame_cb. */
    platform_framebuffer_t *fb = platform_get_framebuffer();
    double scale = platform_get_dpi_scale();
    int fb_x = (int)(mouse_x * scale);
    int fb_y = (int)(mouse_y * scale);

    int off_x, off_y;
    canvas_offset(fb, st, &off_x, &off_y);
    *out_cx = fb_x - off_x;
    *out_cy = fb_y - off_y;
    return *out_cx >= 0 && *out_cx < st->canvas_w
        && *out_cy >= 0 && *out_cy < st->canvas_h;
}

/* Footprint of the current tool: the premultiplied color it lays down and
   how many pixels across. Pencil is fixed at 1px; every other tool tracks
   brush_size. */
static void tool_footprint(const paint_state_t *st, pcolor_t *color, int *size) {
    switch (st->tool) {
    case TOOL_PENCIL: *color = color_premultiply(st->color); *size = 1;              break;
    /* Opaque white, so compositing the eraser is a plain replace. */
    case TOOL_ERASER: *color = CANVAS_BG;                    *size = st->brush_size; break;
    default:          *color = color_premultiply(st->color); *size = st->brush_size; break;
    }
}

/* Mark the current tool's footprint at canvas-pixel (cx, cy). Out-of-bounds
   pixels are clipped, not wrapped. */
static void apply_tool_at(paint_state_t *st, int cx, int cy) {
    pcolor_t color; int size;
    tool_footprint(st, &color, &size);
    (void)color;                      /* coverage now; stroke_composite applies it */
    stamp_square(st, cx, cy, size);
}

/* Lay the finished stroke onto the canvas in a single composite, then clear
   it. This is the one place a stroke's color reaches the canvas, so opacity
   comes out as asked regardless of brush size or mouse speed. */
static void stroke_composite(paint_state_t *st) {
    pcolor_t color; int size;
    tool_footprint(st, &color, &size);
    stroke_blit(st, st->canvas, st->canvas_w, st->canvas_h, 0, 0, color);
    stroke_reset(st);
}

static void stamp_cb(int x, int y, void *ud) {
    apply_tool_at(ud, x, y);
}

/* Stamp the tool along a Bresenham line — fills pixel gaps when the
   mouse moves faster than one event per pixel. Without this, fast
   strokes leave a string of dots instead of a continuous line. */
static void apply_tool_stroke(paint_state_t *st, int x0, int y0, int x1, int y1) {
    draw2d_walk_line(x0, y0, x1, y1, stamp_cb, st);
}

/* Shapes are re-marked from scratch whenever their geometry changes, so the
   mask always holds the current rubber-band. Because the preview draws from
   the same mask that will be composited, what you see while dragging is
   exactly what lands on the canvas. */
static void line_remark(paint_state_t *st) {
    stroke_reset(st);
    draw2d_walk_line(st->line_x0, st->line_y0, st->line_x1, st->line_y1, stamp_cb, st);
}

static void triangle_remark(paint_state_t *st) {
    stroke_reset(st);
    const int *x = st->tri_x, *y = st->tri_y;
    if (st->tri_n == 1) {
        apply_tool_stroke(st, x[0], y[0], x[1], y[1]);
    } else if (st->tri_n == 2) {
        apply_tool_stroke(st, x[0], y[0], x[1], y[1]);
        apply_tool_stroke(st, x[1], y[1], x[2], y[2]);
        apply_tool_stroke(st, x[2], y[2], x[0], y[0]);
    }
}

static void line_commit(paint_state_t *st) {
    line_remark(st);
    stroke_composite(st);
    st->line_active = false;
}

static void triangle_commit(paint_state_t *st) {
    const int *x = st->tri_x, *y = st->tri_y;
    if (st->tool == TOOL_TRIANGLE_WIRE) {
        stroke_reset(st);
        apply_tool_stroke(st, x[0], y[0], x[1], y[1]);
        apply_tool_stroke(st, x[1], y[1], x[2], y[2]);
        apply_tool_stroke(st, x[2], y[2], x[0], y[0]);
        stroke_composite(st);
    } else {
        /* A filled triangle covers each pixel once already, so it can go
           straight onto the canvas without the mask. */
        stroke_reset(st);
        /* The canvas is a bare pixel grid; wrap it so the shared
           rasterizer can write into it. Clipping is per the canvas
           dimensions, so corners dragged into the letterbox are cut. */
        platform_framebuffer_t canvas_fb = {
            /* platform.h knows nothing of pcolor_t, so hand it the raw
               buffer; the rasterizer views it back as pcolor_t. */
            .pixels = (u32 *)st->canvas,
            .width = st->canvas_w, .height = st->canvas_h,
        };
        draw2d_triangle_fill_blend(&canvas_fb, x[0], y[0], x[1], y[1], x[2], y[2],
                                   color_premultiply(st->color));
    }
    st->tri_n = 0;
}

static void canvas_clear(paint_state_t *st) {
    if (!st->canvas) return;
    size_t n = (size_t)st->canvas_w * (size_t)st->canvas_h;
    for (size_t i = 0; i < n; i++) st->canvas[i] = CANVAS_BG;
}

/* Single place that announces the current tool, so the number keys and
   the [ / ] ramp never report it two different ways. The pencil prints no
   width: it is always 1px, and brush_size still holds whatever the eraser
   and shape tools are using. */
static void print_tool(const paint_state_t *st) {
    if (st->tool == TOOL_PENCIL)
        printf("tool: pencil\n");
    else if (tool_is_triangle(st->tool))
        printf("tool: %s (size %d) — click 3 corners, Esc cancels\n",
               tool_name(st->tool), st->brush_size);
    else
        printf("tool: %s (size %d)\n", tool_name(st->tool), st->brush_size);
}

static void set_tool(paint_state_t *st, paint_tool_t t) {
    st->tool        = t;
    st->line_active = false;      /* drop any half-placed shape */
    st->tri_n       = 0;
    print_tool(st);
}

static int clamp_size(int size) {
    if (size < 1)  return 1;
    if (size > 32) return 32;
    return size;
}

/* [ and ] walk one continuous 1..32 width ramp. Pencil and brush are the
   same square stamp differing only in width, so width 1 *is* the pencil and
   2+ is the brush: stepping up off the pencil switches tool rather than
   doing nothing, and stepping the brush down to 1 lands back on the pencil.
   Eraser and the shape tools keep their own plain size adjustment.
   Always echoes — a silent [ / ] reads as a dead key. */
static void adjust_size(paint_state_t *st, int delta) {
    if (st->tool != TOOL_PENCIL && st->tool != TOOL_BRUSH) {
        st->brush_size = clamp_size(st->brush_size + delta);
        printf("size: %d\n", st->brush_size);
        return;
    }

    int current = (st->tool == TOOL_PENCIL) ? 1 : st->brush_size;
    int next    = clamp_size(current + delta);
    if (next <= 1) {
        st->tool = TOOL_PENCIL;        /* brush_size left alone, so the
                                          eraser keeps the width it had */
    } else {
        st->tool       = TOOL_BRUSH;
        st->brush_size = next;
    }
    print_tool(st);
}

/* ---- file I/O ---- */

static const char *const BMP_EXTS[] = { "bmp", NULL };

/* Abandon any half-placed shape: the dialog eats the events that would
   otherwise finish it, and a stroke resuming after a modal feels broken. */
static void cancel_pending(paint_state_t *st) {
    /* A freehand stroke in progress is real work, so keep it; an unfinished
       shape is just a rubber-band, so drop it. */
    if (st->painting) stroke_composite(st);
    else              stroke_reset(st);
    st->painting    = false;
    st->line_active = false;
    st->tri_n       = 0;
}

static void paint_save(paint_state_t *st) {
    char path[1024];
    cancel_pending(st);
    if (!platform_save_dialog("canvas.bmp", BMP_EXTS, path, sizeof path)) {
        printf("save cancelled\n");
        return;
    }
    if (image_save_bmp(path, st->canvas, st->canvas_w, st->canvas_h))
        printf("saved %dx%d to %s\n", st->canvas_w, st->canvas_h, path);
    else
        printf("save FAILED: %s\n", path);
}

/* The canvas takes on the loaded image's dimensions rather than scaling
   the image into the existing one — the window is only a viewport, so a
   larger image just letterboxes differently. */
static void paint_open(paint_state_t *st) {
    char path[1024];
    cancel_pending(st);
    if (!platform_open_dialog(BMP_EXTS, path, sizeof path)) {
        printf("open cancelled\n");
        return;
    }

    pcolor_t *pixels = NULL;
    int  w = 0, h = 0;
    if (!image_load_bmp(path, &pixels, &w, &h)) {
        printf("load FAILED (24/32-bit uncompressed BMP only): %s\n", path);
        return;
    }

    /* Flatten onto white paper. A loaded file can carry real alpha, but the
       canvas is an opaque surface — keeping translucent pixels would let
       the desktop show through the paper and would break the assumption
       that saving produces the image you can see. */
    for (size_t i = 0, n = (size_t)w * (size_t)h; i < n; i++)
        pixels[i] = color_blend(CANVAS_BG, pixels[i]);

    /* The mask is canvas-sized, so a differently-sized image needs a new one. */
    u8 *mask = calloc((size_t)w * (size_t)h, sizeof(u8));
    if (!mask) {
        free(pixels);
        printf("load FAILED (out of memory): %s\n", path);
        return;
    }

    free(st->canvas);
    free(st->stroke);
    st->canvas   = pixels;
    st->stroke   = mask;
    st->canvas_w = w;
    st->canvas_h = h;
    stroke_reset(st);
    printf("loaded %dx%d from %s\n", w, h, path);
}

/* ---- mode callbacks ---- */

static void init(app_mode_t *m) {
    paint_state_t *st = calloc(1, sizeof *st);
    st->tool       = TOOL_PENCIL;
    st->brush_size = 5;
    st->color      = COLOR_RGB(0xFF, 0x88, 0x00);   /* default orange */
    st->last_cx    = -1;
    st->last_cy    = -1;

    /* Canvas at startup framebuffer size (retina-aware). Survives all
       window resizes; the window is just a viewport onto it. */
    platform_framebuffer_t *fb0 = platform_get_framebuffer();
    st->canvas_w = fb0->width;
    st->canvas_h = fb0->height;
    st->canvas   = malloc((size_t)st->canvas_w * (size_t)st->canvas_h * sizeof *st->canvas);
    st->stroke   = calloc((size_t)st->canvas_w * (size_t)st->canvas_h, sizeof(u8));
    canvas_clear(st);
    stroke_reset(st);                 /* seeds the empty dirty rect */

    m->state = st;
}

static void cleanup(app_mode_t *m) {
    paint_state_t *st = m->state;
    free(st->canvas);
    free(st->stroke);
    free(st);
    m->state = NULL;
}

static void leave(app_mode_t *m) {
    cancel_pending(m->state);         /* don't resume a stroke on re-enter */
}

static void event(app_mode_t *m, const platform_event_t *e) {
    paint_state_t *st = m->state;
    switch (e->kind) {
    case PLATFORM_EV_KEY_DOWN:
        if (e->key.repeat || !platform_key_is_plain(e)) break;
        switch (e->key.key) {
        case PLATFORM_KEY_1: set_tool(st, TOOL_PENCIL); break;
        case PLATFORM_KEY_2: set_tool(st, TOOL_BRUSH);  break;
        case PLATFORM_KEY_3: set_tool(st, TOOL_ERASER); break;
        case PLATFORM_KEY_4: set_tool(st, TOOL_LINE);   break;
        case PLATFORM_KEY_5: set_tool(st, TOOL_TRIANGLE);      break;
        case PLATFORM_KEY_6: set_tool(st, TOOL_TRIANGLE_WIRE); break;
        case PLATFORM_KEY_ESCAPE:
            if (st->line_active) { st->line_active = false; printf("line cancelled\n"); }
            if (st->tri_n)       { st->tri_n = 0;           printf("triangle cancelled\n"); }
            stroke_reset(st);          /* discard the uncommitted preview */
            break;
        case PLATFORM_KEY_LEFT_BRACKET:  adjust_size(st, -1); break;
        case PLATFORM_KEY_RIGHT_BRACKET: adjust_size(st, +1); break;
        case PLATFORM_KEY_C:
            canvas_clear(st);
            printf("canvas cleared\n");
            break;
        case PLATFORM_KEY_S: paint_save(st); break;
        case PLATFORM_KEY_O: paint_open(st); break;
        default: break;
        }
        break;

    case PLATFORM_EV_MOUSE_DOWN: {
        if (e->mouse.btn != PLATFORM_MOUSE_LEFT) break;
        int cx, cy;
        bool inside = mouse_to_canvas(st, e->mouse.x, e->mouse.y, &cx, &cy);

        /* Corners may be placed anywhere, including the letterbox — the
           rasterizer clips to the canvas on commit. */
        if (tool_is_triangle(st->tool)) {
            st->tri_x[st->tri_n] = cx;
            st->tri_y[st->tri_n] = cy;
            st->tri_n++;
            if (st->tri_n == 3) {
                triangle_commit(st);
            } else {
                st->tri_x[st->tri_n] = cx;   /* seed live corner */
                st->tri_y[st->tri_n] = cy;
                triangle_remark(st);
            }
            break;
        }

        if (!inside) break;
        if (st->tool == TOOL_LINE) {
            st->line_active = true;
            st->line_x0 = st->line_x1 = cx;
            st->line_y0 = st->line_y1 = cy;
            line_remark(st);
            break;
        }
        st->painting = true;
        st->last_cx  = cx;
        st->last_cy  = cy;
        stroke_reset(st);              /* one mask per freehand stroke */
        apply_tool_at(st, cx, cy);
    } break;

    case PLATFORM_EV_MOUSE_UP:
        if (e->mouse.btn != PLATFORM_MOUSE_LEFT) break;
        if (st->line_active) {
            mouse_to_canvas(st, e->mouse.x, e->mouse.y, &st->line_x1, &st->line_y1);
            line_commit(st);
        } else if (st->painting) {
            stroke_composite(st);      /* the stroke reaches the canvas here */
        }
        st->painting = false;
        break;

    case PLATFORM_EV_MOUSE_MOVE: {
        if (tool_is_triangle(st->tool)) {
            if (st->tri_n > 0) {
                mouse_to_canvas(st, e->move.x, e->move.y,
                                &st->tri_x[st->tri_n], &st->tri_y[st->tri_n]);
                triangle_remark(st);
            }
            break;
        }
        if (st->line_active) {
            mouse_to_canvas(st, e->move.x, e->move.y, &st->line_x1, &st->line_y1);
            line_remark(st);
            break;
        }
        if (!st->painting) break;
        int cx, cy;
        if (!mouse_to_canvas(st, e->move.x, e->move.y, &cx, &cy)) {
            /* Cursor left the canvas mid-stroke — drop the segment but
               don't end the stroke; re-entry starts fresh from the new
               position rather than drawing a line across the gap. */
            st->last_cx = -1;
            st->last_cy = -1;
            break;
        }
        if (st->last_cx < 0) apply_tool_at(st, cx, cy);
        else                 apply_tool_stroke(st, st->last_cx, st->last_cy, cx, cy);
        st->last_cx = cx;
        st->last_cy = cy;
    } break;

    default:
        break;
    }
}

static void frame(app_mode_t *m, platform_framebuffer_t *fb) {
    paint_state_t *st = m->state;
    render_canvas(fb, st);

    /* The pending stroke lives only in the mask until it is composited, so
       draw it over the canvas to show it in progress. Same mask, same
       single blend — the preview matches the final result exactly. */
    if (!stroke_is_empty(st)) {
        int off_x, off_y;
        canvas_offset(fb, st, &off_x, &off_y);
        pcolor_t color; int size;
        tool_footprint(st, &color, &size);
        stroke_blit(st, pcolor_pixels(fb->pixels), fb->width, fb->height,
                    off_x, off_y, color);
    }
}

static void set_color(app_mode_t *m, color_t c) {
    paint_state_t *st = m->state;
    st->color = c;
    printf("paint color: 0x%08X\n", c.rgba);
}

app_mode_t paint_mode(void) {
    return (app_mode_t){
        .name      = "paint",
        .init      = init,
        .cleanup   = cleanup,
        .leave     = leave,
        .event     = event,
        .frame     = frame,
        .set_color = set_color,
    };
}
