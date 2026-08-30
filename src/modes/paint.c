#include "modes/paint.h"
#include "render/color.h"
#include "render/draw2d.h"
#include "render/framebuffer.h"
#include "render/geom.h"
#include "util/image.h"
#include "ui/ui.h"
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
    vec2_t       last;              /* prev stroke point, canvas coords; x < 0 = none */

    /* line tool: rubber-band from anchor to cursor while the button is
       held; committed to the canvas on release. Coords may lie outside
       the canvas — stamping clips per pixel. */
    bool         line_active;
    line2d_t     line;              /* a = anchor, b = cursor */

    /* triangle tool: click-click-click. tri_n counts committed corners
       (0..2); slot [tri_n] tracks the cursor so the rubber-band preview
       always has a live third point. The 3rd click commits and resets. */
    int          tri_n;
    vec2_t       tri[3];

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

    /* Panels are retained so closing one and reopening it restores its
       place; the ui_t itself only carries a frame's worth of input. */
    ui_t       ui;
    ui_panel_t panel_tools, panel_brush, panel_colors, panel_actions;
} paint_state_t;

#define CANVAS_BG PCOLOR_RGB(0xFF, 0xFF, 0xFF)

/* Letterbox offset of the canvas inside the framebuffer. Single source
   of truth — render, hit-testing and the line preview all use it, so
   the cursor always lands exactly on the painted pixel. */
static vec2_t canvas_offset(const platform_framebuffer_t *fb, const paint_state_t *st) {
    return vec2((fb->width  - st->canvas_w) / 2,
                (fb->height - st->canvas_h) / 2);
}

/* Fill a size×size square anchored on (cx,cy) into any pixel grid, clipped
   to [0,w)×[0,h). Width is exactly `size`: a 4 really is 4px across, sitting
   half a pixel off centre. Deriving a radius as size/2 instead would make
   every even size identical to the odd one below it, so half the [ / ]
   presses would change nothing on screen.

   Marks coverage only — the color is applied later, once, by
   stroke_composite. */
static void stamp_square(paint_state_t *st, vec2_t c, int size) {
    int x0 = c.x - (size - 1) / 2, x1 = x0 + size - 1;
    int y0 = c.y - (size - 1) / 2, y1 = y0 + size - 1;
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

    vec2_t off = canvas_offset(fb, st);
    int off_x = off.x, off_y = off.y;

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
                            vec2_t *out) {
    /* Only width/height are read here — safe outside frame_cb. */
    platform_framebuffer_t *fb = platform_get_framebuffer();
    double scale = platform_get_dpi_scale();
    vec2_t fb_p = vec2((int)(mouse_x * scale), (int)(mouse_y * scale));

    *out = vec2_sub(fb_p, canvas_offset(fb, st));
    return rect2d_contains(rect2d(0, 0, st->canvas_w, st->canvas_h), *out);
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
static void apply_tool_at(paint_state_t *st, vec2_t p) {
    pcolor_t color; int size;
    tool_footprint(st, &color, &size);
    (void)color;                      /* coverage now; stroke_composite applies it */
    stamp_square(st, p, size);
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

static void stamp_tool_pixel(int x, int y, void *state) {
    apply_tool_at(state, vec2(x, y));
}

/* Stamp the tool along a Bresenham line — fills pixel gaps when the
   mouse moves faster than one event per pixel. Without this, fast
   strokes leave a string of dots instead of a continuous line. */
static void apply_tool_stroke(paint_state_t *st, line2d_t l) {
    draw2d_walk_line(l, stamp_tool_pixel, st);
}

/* Mark a single covered pixel. The shape rasterizers report interior
   coverage one pixel at a time; the brush footprint applies to the outline
   tools only, so this stamps width 1. */
static void mark_coverage_pixel(int x, int y, void *state) {
    stamp_square(state, vec2(x, y), 1);
}

/* Shapes are re-marked from scratch whenever their geometry changes, so the
   mask always holds the current rubber-band. Because the preview draws from
   the same mask that will be composited, what you see while dragging is
   exactly what lands on the canvas. */
static void line_remark(paint_state_t *st) {
    stroke_reset(st);
    apply_tool_stroke(st, st->line);
}

/* The complete three-corner shape: interior for TOOL_TRIANGLE, outline for
   TOOL_TRIANGLE_WIRE. Preview and commit both mark through here, so a
   filled triangle no longer previews as an outline and then commits as a
   fill. Marking coverage rather than blending keeps the fill single-blended
   exactly as the direct rasterizer call did. */
static void triangle_mark_full(paint_state_t *st) {
    const vec2_t *p = st->tri;
    /* Clipped to the canvas, so corners dragged into the letterbox are cut. */
    if (st->tool == TOOL_TRIANGLE
        && draw2d_walk_triangle(tri2d(p[0], p[1], p[2]),
                                rect2d(0, 0, st->canvas_w, st->canvas_h),
                                mark_coverage_pixel, st))
        return;

    /* Outline, for the wire tool and for a filled triangle with no interior.
       Collinear corners have zero area, and the live corner is seeded on top
       of the one just placed, so a filled triangle is degenerate every time a
       corner lands and whenever the cursor crosses the line through the other
       two. Marking nothing there would blink the rubber-band out instead of
       degenerating to the line the shape actually is. */
    apply_tool_stroke(st, line2d(p[0], p[1]));
    apply_tool_stroke(st, line2d(p[1], p[2]));
    apply_tool_stroke(st, line2d(p[2], p[0]));
}

static void triangle_remark(paint_state_t *st) {
    stroke_reset(st);
    /* One corner placed: no interior exists yet, so track the cursor with a
       plain edge whichever triangle tool is selected. */
    if (st->tri_n == 1)      apply_tool_stroke(st, line2d(st->tri[0], st->tri[1]));
    else if (st->tri_n == 2) triangle_mark_full(st);
}

static void line_commit(paint_state_t *st) {
    line_remark(st);
    stroke_composite(st);
    st->line_active = false;
}

static void triangle_commit(paint_state_t *st) {
    stroke_reset(st);
    triangle_mark_full(st);
    stroke_composite(st);
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

static void cancel_pending(paint_state_t *st);

/* Switching tools ends whatever was in flight. cancel_pending runs *before*
   st->tool changes: it composites an in-progress freehand stroke, and
   stroke_composite reads the colour and width from tool_footprint, so
   switching first would commit the stroke in the new tool's colour.
   Clearing only line_active/tri_n here used to leave the mask marked, and
   frame() kept blitting that dead rubber-band every frame until an
   unrelated click happened to reset it. */
static void set_tool(paint_state_t *st, paint_tool_t t) {
    cancel_pending(st);
    st->tool = t;
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
static int current_size(const paint_state_t *st) {
    return st->tool == TOOL_PENCIL ? 1 : st->brush_size;
}

/* The ramp lives here alone, so the [ / ] keys and the brush slider cannot
   disagree about what width 1 means. */
static void set_size(paint_state_t *st, int size) {
    if (st->tool != TOOL_PENCIL && st->tool != TOOL_BRUSH) {
        st->brush_size = clamp_size(size);
        printf("size: %d\n", st->brush_size);
        return;
    }

    int next = clamp_size(size);
    if (next <= 1) {
        st->tool = TOOL_PENCIL;        /* brush_size left alone, so the
                                          eraser keeps the width it had */
    } else {
        st->tool       = TOOL_BRUSH;
        st->brush_size = next;
    }
    print_tool(st);
}

static void adjust_size(paint_state_t *st, int delta) {
    set_size(st, (st->tool != TOOL_PENCIL && st->tool != TOOL_BRUSH)
                     ? st->brush_size + delta
                     : current_size(st) + delta);
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

/* ---- commands ---- */

/* Esc: drop a half-placed shape and the rubber-band preview it was
   drawing. Distinct from cancel_pending(), which also commits a freehand
   stroke that is already partly real. */
static void cancel_shape(paint_state_t *st) {
    if (st->line_active) { st->line_active = false; printf("line cancelled\n"); }
    if (st->tri_n)       { st->tri_n = 0;           printf("triangle cancelled\n"); }
    /* A freehand stroke has to end here too. Leaving painting set kept the
       stroke live with its mask already discarded, so the next mouse move
       resumed stamping from the stale st->last and drew a segment from
       wherever the cursor sat at Esc. */
    st->painting = false;
    st->last     = vec2(-1, -1);
    stroke_reset(st);              /* discard the uncommitted preview */
}

/* Every action the mode can perform, named once. Keys dispatch through
   paint_exec and so will the toolbar, so an action can never end up
   reachable one way but not the other. No CMD_COUNT and no default in the
   switch below: -Wswitch then flags any command added without a body. */
typedef enum {
    CMD_TOOL_PENCIL,
    CMD_TOOL_BRUSH,
    CMD_TOOL_ERASER,
    CMD_TOOL_LINE,
    CMD_TOOL_TRIANGLE,
    CMD_TOOL_TRIANGLE_WIRE,
    CMD_SIZE_DEC,
    CMD_SIZE_INC,
    CMD_CANCEL,
    CMD_CLEAR,
    CMD_SAVE,
    CMD_OPEN,
} paint_cmd_t;

/* Which panel a command's button belongs to. */
typedef enum { GROUP_TOOL, GROUP_SIZE, GROUP_ACTION } paint_group_t;

typedef struct {
    paint_cmd_t    cmd;
    const char    *label;      /* button text */
    paint_group_t  group;      /* panel the button lands in */
    platform_key_t key;
    u32            mods;       /* OR of platform_mod_t; 0 = unmodified key */
} paint_binding_t;

/* Bindings as data, in the order the toolbar lays them out. Every command
   has a row, so nothing can end up keyboard-only. */
static const paint_binding_t PAINT_COMMANDS[] = {
    { CMD_TOOL_PENCIL,        "Pencil",   GROUP_TOOL,   PLATFORM_KEY_1,             0 },
    { CMD_TOOL_BRUSH,         "Brush",    GROUP_TOOL,   PLATFORM_KEY_2,             0 },
    { CMD_TOOL_ERASER,        "Eraser",   GROUP_TOOL,   PLATFORM_KEY_3,             0 },
    { CMD_TOOL_LINE,          "Line",     GROUP_TOOL,   PLATFORM_KEY_4,             0 },
    { CMD_TOOL_TRIANGLE,      "Triangle", GROUP_TOOL,   PLATFORM_KEY_5,             0 },
    { CMD_TOOL_TRIANGLE_WIRE, "Tri Wire", GROUP_TOOL,   PLATFORM_KEY_6,             0 },
    { CMD_SIZE_DEC,           "Smaller",  GROUP_SIZE,   PLATFORM_KEY_LEFT_BRACKET,  0 },
    { CMD_SIZE_INC,           "Bigger",   GROUP_SIZE,   PLATFORM_KEY_RIGHT_BRACKET, 0 },
    { CMD_CANCEL,             "Cancel",   GROUP_ACTION, PLATFORM_KEY_ESCAPE,        0 },
    { CMD_CLEAR,              "Clear",    GROUP_ACTION, PLATFORM_KEY_C,             0 },
    { CMD_SAVE,               "Save",     GROUP_ACTION, PLATFORM_KEY_S,             0 },
    { CMD_OPEN,               "Open",     GROUP_ACTION, PLATFORM_KEY_O,             0 },
};

/* Palette offered by the Colors panel. The last entry is deliberately
   translucent: paint opacity is the colour's alpha, so the panel should
   make that reachable without typing hex at the '#' prompt. */
static const color_t PALETTE[] = {
    COLOR_RGB(0x00, 0x00, 0x00), COLOR_RGB(0x7F, 0x7F, 0x7F),
    COLOR_RGB(0xFF, 0xFF, 0xFF), COLOR_RGB(0xC0, 0x00, 0x00),
    COLOR_RGB(0xFF, 0x00, 0x00), COLOR_RGB(0xFF, 0x88, 0x00),
    COLOR_RGB(0xFF, 0xE0, 0x00), COLOR_RGB(0x00, 0xA0, 0x00),
    COLOR_RGB(0x00, 0xE0, 0x40), COLOR_RGB(0x00, 0x60, 0xC0),
    COLOR_RGB(0x00, 0xC0, 0xFF), COLOR_RGB(0x60, 0x00, 0xC0),
    COLOR_RGB(0xC0, 0x00, 0xC0), COLOR_RGB(0x80, 0x40, 0x00),
    COLOR_RGB(0xFF, 0xC0, 0xA0), COLOR_RGBA(0xFF, 0x00, 0x00, 0x60),
};

/* Shared by the shell's '#' overlay and the Colors panel, so both report
   the change the same way. */
static void apply_color(paint_state_t *st, color_t c) {
    st->color = c;
    printf("paint color: 0x%08X\n", c.rgba);
}

/* Buttons read state rather than remembering it, so a tool picked with the
   keyboard lights its button up with no extra plumbing. */
static bool cmd_is_selected(const paint_state_t *st, paint_cmd_t cmd) {
    switch (cmd) {
    case CMD_TOOL_PENCIL:        return st->tool == TOOL_PENCIL;
    case CMD_TOOL_BRUSH:         return st->tool == TOOL_BRUSH;
    case CMD_TOOL_ERASER:        return st->tool == TOOL_ERASER;
    case CMD_TOOL_LINE:          return st->tool == TOOL_LINE;
    case CMD_TOOL_TRIANGLE:      return st->tool == TOOL_TRIANGLE;
    case CMD_TOOL_TRIANGLE_WIRE: return st->tool == TOOL_TRIANGLE_WIRE;
    default:                     return false;
    }
}

static void paint_exec(paint_state_t *st, paint_cmd_t cmd) {
    switch (cmd) {
    case CMD_TOOL_PENCIL:        set_tool(st, TOOL_PENCIL);        break;
    case CMD_TOOL_BRUSH:         set_tool(st, TOOL_BRUSH);         break;
    case CMD_TOOL_ERASER:        set_tool(st, TOOL_ERASER);        break;
    case CMD_TOOL_LINE:          set_tool(st, TOOL_LINE);          break;
    case CMD_TOOL_TRIANGLE:      set_tool(st, TOOL_TRIANGLE);      break;
    case CMD_TOOL_TRIANGLE_WIRE: set_tool(st, TOOL_TRIANGLE_WIRE); break;
    case CMD_SIZE_DEC:           adjust_size(st, -1);              break;
    case CMD_SIZE_INC:           adjust_size(st, +1);              break;
    case CMD_CANCEL:             cancel_shape(st);                 break;
    /* cancel_pending first: without it the mask outlived the clear and
       composited itself onto the fresh canvas on release, so "clear" left
       a stroke behind. */
    case CMD_CLEAR:              cancel_pending(st); canvas_clear(st);
                                 printf("canvas cleared\n");     break;
    case CMD_SAVE:               paint_save(st);                   break;
    case CMD_OPEN:               paint_open(st);                   break;
    }
}

/* ---- UI layout ---- */

/* The last panel's y plus its height, in the design units the positions
   below use. Adjacent to them so the two cannot drift apart. */
#define PANEL_COLUMN_BOTTOM 480

/* Both UI scales follow the DISPLAY THE WINDOW IS ON, so they are derived
   here and re-derived on every resize rather than captured once at init.

   Captured once, they go stale the moment the window is dragged to a
   screen with a different backing scale. The widgets keeping their old
   size is the visible half; the damaging half is device_scale, which
   converts the mouse points the platform reports into the framebuffer
   pixels the widgets occupy. Stale by a factor of two, a click on the
   second button in a panel selects the sixth.

   device_scale is the floor for the widget size, not the whole story:
   matching it exactly makes the UI physically the size it would be on a
   1x display, which on a dense screen is a 68pt column of 8pt text. Every
   metric is an integer multiple of a bitmap grid, so "bigger" means the
   next whole step up, and the ceiling is the bottom panel staying on
   screen. */
/* Position and size only. `open` and `last_h` are deliberately untouched:
   a relayout must not reopen a panel the user closed. */
static void place_panel(ui_panel_t *p, const char *title, int x, int y) {
    p->title = title;
    p->pos   = vec2(x, y);
    p->w     = 68;
}

static void relayout_ui(paint_state_t *st, int fb_h, int device_scale) {
    if (device_scale < 1) device_scale = 1;

    int s = device_scale;
    while ((s + 1) * PANEL_COLUMN_BOTTOM <= fb_h) s++;

    /* Ordinary resizes must not move the panels — a window drag would
       otherwise throw away wherever the user put them. Only a change of
       scale, which invalidates the positions anyway, relays them out. */
    if (s == st->ui.scale && device_scale == st->ui.device_scale) return;

    printf("display: backing scale %d, framebuffer %dpx tall -> ui scale %d\n",
           device_scale, fb_h, s);

    st->ui.scale        = s;
    st->ui.device_scale = device_scale;

    /* Stacked down the left edge with a gap between each, sized from the
       content they hold — a panel's height is its widgets, so these have
       to be spaced by hand rather than flowed. */
    place_panel(&st->panel_tools,   "Tools",   12 * s,  20 * s);
    place_panel(&st->panel_brush,   "Brush",   12 * s, 152 * s);
    place_panel(&st->panel_colors,  "Colors",  12 * s, 244 * s);
    place_panel(&st->panel_actions, "Actions", 12 * s, 387 * s);
}

/* ---- mode callbacks ---- */

static void init(app_mode_t *m) {
    paint_state_t *st = calloc(1, sizeof *st);
    st->tool       = TOOL_PENCIL;
    st->brush_size = 5;
    st->color      = COLOR_RGB(0xFF, 0x88, 0x00);   /* default orange */
    st->last       = vec2(-1, -1);

    /* Canvas at startup framebuffer size (retina-aware). Survives all
       window resizes; the window is just a viewport onto it. */
    platform_framebuffer_t *fb0 = platform_get_framebuffer();
    st->canvas_w = fb0->width;
    st->canvas_h = fb0->height;
    st->canvas   = malloc((size_t)st->canvas_w * (size_t)st->canvas_h * sizeof *st->canvas);
    st->stroke   = calloc((size_t)st->canvas_w * (size_t)st->canvas_h, sizeof(u8));
    canvas_clear(st);
    stroke_reset(st);                 /* seeds the empty dirty rect */

    st->panel_tools.open   = true;
    st->panel_brush.open   = true;
    st->panel_colors.open  = true;
    st->panel_actions.open = true;
    relayout_ui(st, fb0->height, (int)(platform_get_dpi_scale() + 0.5));

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

    /* The UI sees the mouse first and swallows what it uses, so a click on
       a button never also lands on the canvas underneath it. A stroke
       already in progress keeps its capture: ui_event only claims presses
       that start on a panel. */
    if (ui_event(&st->ui, e)) return;

    switch (e->kind) {
    case PLATFORM_EV_KEY_DOWN:
        if (e->key.repeat) break;
        /* Mods must match exactly rather than as a subset. Every binding
           is unmodified, so a modified press matches nothing — the same
           guard platform_key_is_plain() gave, and what keeps Shift+3
           ('#', the shell's colour input) from also picking the eraser. */
        for (int i = 0; i < ARRAY_COUNT(PAINT_COMMANDS); i++) {
            if (e->key.key == PAINT_COMMANDS[i].key &&
                e->key.mods == PAINT_COMMANDS[i].mods) {
                paint_exec(st, PAINT_COMMANDS[i].cmd);
                break;
            }
        }
        break;

    case PLATFORM_EV_MOUSE_DOWN: {
        if (e->mouse.btn != PLATFORM_MOUSE_LEFT) break;
        vec2_t p;
        bool inside = mouse_to_canvas(st, e->mouse.x, e->mouse.y, &p);

        /* Corners may be placed anywhere, including the letterbox — the
           rasterizer clips to the canvas on commit. */
        if (tool_is_triangle(st->tool)) {
            st->tri[st->tri_n] = p;
            st->tri_n++;
            if (st->tri_n == 3) {
                triangle_commit(st);
            } else {
                st->tri[st->tri_n] = p;      /* seed live corner */
                triangle_remark(st);
            }
            break;
        }

        if (!inside) break;
        if (st->tool == TOOL_LINE) {
            st->line_active = true;
            st->line = line2d(p, p);
            line_remark(st);
            break;
        }
        st->painting = true;
        st->last     = p;
        stroke_reset(st);              /* one mask per freehand stroke */
        apply_tool_at(st, p);
    } break;

    case PLATFORM_EV_MOUSE_UP:
        if (e->mouse.btn != PLATFORM_MOUSE_LEFT) break;
        if (st->line_active) {
            mouse_to_canvas(st, e->mouse.x, e->mouse.y, &st->line.b);
            line_commit(st);
        } else if (st->painting) {
            stroke_composite(st);      /* the stroke reaches the canvas here */
        }
        st->painting = false;
        break;

    case PLATFORM_EV_MOUSE_MOVE: {
        if (tool_is_triangle(st->tool)) {
            if (st->tri_n > 0) {
                mouse_to_canvas(st, e->move.x, e->move.y, &st->tri[st->tri_n]);
                triangle_remark(st);
            }
            break;
        }
        if (st->line_active) {
            mouse_to_canvas(st, e->move.x, e->move.y, &st->line.b);
            line_remark(st);
            break;
        }
        if (!st->painting) break;
        vec2_t p;
        if (!mouse_to_canvas(st, e->move.x, e->move.y, &p)) {
            /* Cursor left the canvas mid-stroke — drop the segment but
               don't end the stroke; re-entry starts fresh from the new
               position rather than drawing a line across the gap. */
            st->last.x = -1;
            break;
        }
        if (st->last.x < 0) apply_tool_at(st, p);
        else                apply_tool_stroke(st, line2d(st->last, p));
        st->last = p;
    } break;

    /* Fires on a window resize AND on crossing to a display with a
       different backing scale — the case that matters here, since both UI
       scales are derived from it. The event carries both spaces, so the
       backing scale is their ratio rather than a second platform query. */
    case PLATFORM_EV_RESIZE:
        if (e->resize.w > 0)
            relayout_ui(st, e->resize.fb_h, e->resize.fb_w / e->resize.w);
        break;

    default:
        break;
    }
}

/* Buttons for one group, in table order. */
static void group_buttons(paint_state_t *st, paint_group_t group) {
    for (int i = 0; i < ARRAY_COUNT(PAINT_COMMANDS); i++) {
        const paint_binding_t *b = &PAINT_COMMANDS[i];
        if (b->group != group) continue;
        if (ui_button(&st->ui, b->label, cmd_is_selected(st, b->cmd)))
            paint_exec(st, b->cmd);
    }
}

static void draw_ui(paint_state_t *st, platform_framebuffer_t *fb) {
    ui_t *ui = &st->ui;
    ui_begin_frame(ui, fb, ui->scale);

    if (ui_panel_begin(ui, &st->panel_tools)) {
        group_buttons(st, GROUP_TOOL);
        ui_panel_end(ui);
    }

    if (ui_panel_begin(ui, &st->panel_brush)) {
        /* The slider goes through set_size for the same reason the keys do
           — the pencil/brush ramp has exactly one implementation. */
        int size = current_size(st);
        if (ui_slider(ui, "Size", &size, 1, 32)) set_size(st, size);
        group_buttons(st, GROUP_SIZE);
        ui_panel_end(ui);
    }

    if (ui_panel_begin(ui, &st->panel_colors)) {
        char hex[16];
        snprintf(hex, sizeof hex, "%08X", st->color.rgba);
        ui_label(ui, hex);
        int picked = -1;
        int selected = -1;
        for (int i = 0; i < ARRAY_COUNT(PALETTE); i++)
            if (PALETTE[i].rgba == st->color.rgba) selected = i;
        if (ui_swatches(ui, PALETTE, ARRAY_COUNT(PALETTE), selected, &picked))
            apply_color(st, PALETTE[picked]);
        ui_panel_end(ui);
    }

    if (ui_panel_begin(ui, &st->panel_actions)) {
        group_buttons(st, GROUP_ACTION);
        ui_panel_end(ui);
    }

    /* Last, so an open dropdown lands over the panels rather than under
       them — and so a closed panel has a way back. */
    ui_panel_t *panels[] = {
        &st->panel_tools, &st->panel_brush, &st->panel_colors, &st->panel_actions,
    };
    ui_menubar_begin(ui);
    if (ui_menu_begin(ui, "Panels", ARRAY_COUNT(panels))) {
        for (int i = 0; i < ARRAY_COUNT(panels); i++)
            if (ui_menu_item(ui, panels[i]->title, panels[i]->open))
                panels[i]->open = !panels[i]->open;
        ui_menu_end(ui);
    }
    ui_menubar_end(ui);

    ui_end_frame(ui);
}

static void frame(app_mode_t *m, platform_framebuffer_t *fb) {
    paint_state_t *st = m->state;
    render_canvas(fb, st);

    /* The pending stroke lives only in the mask until it is composited, so
       draw it over the canvas to show it in progress. Same mask, same
       single blend — the preview matches the final result exactly. */
    if (!stroke_is_empty(st)) {
        vec2_t off = canvas_offset(fb, st);
        pcolor_t color; int size;
        tool_footprint(st, &color, &size);
        stroke_blit(st, pcolor_pixels(fb->pixels), fb->width, fb->height,
                    off.x, off.y, color);
    }

    draw_ui(st, fb);                  /* panels sit above the canvas */
}

static void set_color(app_mode_t *m, color_t c) {
    apply_color(m->state, c);
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
