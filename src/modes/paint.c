#include "modes/paint.h"
#include "render/draw2d.h"
#include "render/framebuffer.h"
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
    u32 *canvas;
    int  canvas_w, canvas_h;

    paint_tool_t tool;              /* default TOOL_PENCIL */
    int          brush_size;        /* 1..32, default 5 */
    u32          color;             /* straight ARGB from the shell's # input */
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
} paint_state_t;

#define CANVAS_BG 0xFFFFFFFFu

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
   presses would change nothing on screen. */
static void stamp_square(u32 *pixels, int w, int h, int cx, int cy, int size, u32 color) {
    int x0 = cx - (size - 1) / 2, x1 = x0 + size - 1;
    int y0 = cy - (size - 1) / 2, y1 = y0 + size - 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= w) x1 = w - 1;
    if (y1 >= h) y1 = h - 1;
    for (int y = y0; y <= y1; y++) {
        u32 *row = &pixels[y * w];
        for (int x = x0; x <= x1; x++) row[x] = color;
    }
}

/* ---- render: letterbox the canvas inside the framebuffer ----
   Window > canvas → gray bars; window < canvas → canvas pixels are
   clipped from view but remain in memory. No interpolation, ever.
   Invariant: this and mouse_to_canvas use the same offset formula
   so the cursor lands exactly on the painted pixel. */

static void render_canvas(platform_framebuffer_t *fb, const paint_state_t *st) {
    const u32 *canvas = st->canvas;
    int canvas_w = st->canvas_w, canvas_h = st->canvas_h;

    framebuffer_clear(fb, RGB(48, 48, 48));
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
    size_t row_bytes = (size_t)(dst_x1 - dst_x0) * sizeof(u32);
    for (int y = dst_y0; y < dst_y1; y++) {
        memcpy(&fb->pixels[y * fb->width + dst_x0],
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
            fb->pixels[top * fb->width + x] = RGB(0, 0, 0);
    }
    if (bot >= 0 && bot < fb->height) {
        for (int x = hx0; x <= hx1; x++)
            fb->pixels[bot * fb->width + x] = RGB(0, 0, 0);
    }
    if (left >= 0 && left < fb->width) {
        for (int y = vy0; y <= vy1; y++)
            fb->pixels[y * fb->width + left] = RGB(0, 0, 0);
    }
    if (right >= 0 && right < fb->width) {
        for (int y = vy0; y <= vy1; y++)
            fb->pixels[y * fb->width + right] = RGB(0, 0, 0);
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

/* Footprint of the current tool: what color it lays down and how many
   pixels across. Pencil is fixed at 1px; every other tool tracks brush_size. */
static void tool_footprint(const paint_state_t *st, u32 *color, int *size) {
    switch (st->tool) {
    case TOOL_PENCIL: *color = st->color; *size = 1;              break;
    case TOOL_ERASER: *color = CANVAS_BG; *size = st->brush_size; break;
    default:          *color = st->color; *size = st->brush_size; break;
    }
}

/* Apply current tool centered at canvas-pixel (cx, cy). Out-of-bounds
   pixels in the footprint are clipped, not wrapped. */
static void apply_tool_at(paint_state_t *st, int cx, int cy) {
    u32 color; int size;
    tool_footprint(st, &color, &size);
    stamp_square(st->canvas, st->canvas_w, st->canvas_h, cx, cy, size, color);
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

/* Line-tool preview: stamp the footprint straight into the framebuffer
   at canvas→fb offset, so the canvas itself is untouched until release. */
typedef struct {
    platform_framebuffer_t *fb;
    int off_x, off_y;
    int size;
    u32 color;
} preview_ctx_t;

static void preview_cb(int x, int y, void *ud) {
    preview_ctx_t *c = ud;
    stamp_square(c->fb->pixels, c->fb->width, c->fb->height,
                 x + c->off_x, y + c->off_y, c->size, c->color);
}

static void preview_segment(platform_framebuffer_t *fb, const paint_state_t *st,
                            int x0, int y0, int x1, int y1) {
    preview_ctx_t c = { .fb = fb };
    canvas_offset(fb, st, &c.off_x, &c.off_y);
    tool_footprint(st, &c.color, &c.size);
    draw2d_walk_line(x0, y0, x1, y1, preview_cb, &c);
}

static void render_line_preview(platform_framebuffer_t *fb, const paint_state_t *st) {
    preview_segment(fb, st, st->line_x0, st->line_y0, st->line_x1, st->line_y1);
}

static void line_commit(paint_state_t *st) {
    draw2d_walk_line(st->line_x0, st->line_y0, st->line_x1, st->line_y1, stamp_cb, st);
    st->line_active = false;
}

/* Outline preview for both triangle tools — the fill only appears on
   commit, so an in-progress shape always reads as in-progress. */
static void render_triangle_preview(platform_framebuffer_t *fb, const paint_state_t *st) {
    const int *x = st->tri_x, *y = st->tri_y;
    if (st->tri_n == 1) {
        preview_segment(fb, st, x[0], y[0], x[1], y[1]);
    } else if (st->tri_n == 2) {
        preview_segment(fb, st, x[0], y[0], x[1], y[1]);
        preview_segment(fb, st, x[1], y[1], x[2], y[2]);
        preview_segment(fb, st, x[2], y[2], x[0], y[0]);
    }
}

static void triangle_commit(paint_state_t *st) {
    const int *x = st->tri_x, *y = st->tri_y;
    if (st->tool == TOOL_TRIANGLE_WIRE) {
        apply_tool_stroke(st, x[0], y[0], x[1], y[1]);
        apply_tool_stroke(st, x[1], y[1], x[2], y[2]);
        apply_tool_stroke(st, x[2], y[2], x[0], y[0]);
    } else {
        /* The canvas is a bare pixel grid; wrap it so the shared
           rasterizer can write into it. Clipping is per the canvas
           dimensions, so corners dragged into the letterbox are cut. */
        platform_framebuffer_t canvas_fb = {
            .pixels = st->canvas, .width = st->canvas_w, .height = st->canvas_h,
        };
        draw2d_triangle_fill(&canvas_fb, x[0], y[0], x[1], y[1], x[2], y[2], st->color);
    }
    st->tri_n = 0;
}

static void canvas_clear(paint_state_t *st) {
    if (!st->canvas) return;
    size_t n = (size_t)st->canvas_w * (size_t)st->canvas_h;
    for (size_t i = 0; i < n; i++) st->canvas[i] = CANVAS_BG;
}

static void set_tool(paint_state_t *st, paint_tool_t t) {
    st->tool        = t;
    st->line_active = false;      /* drop any half-placed shape */
    st->tri_n       = 0;
    if (t == TOOL_PENCIL)       printf("tool: pencil\n");
    else if (tool_is_triangle(t)) printf("tool: %s (size %d) — click 3 corners, Esc cancels\n",
                                         tool_name(t), st->brush_size);
    else                        printf("tool: %s (size %d)\n", tool_name(t), st->brush_size);
}

/* Always echoes, whatever the tool — a silent [ / ] reads as a dead key. */
static void set_brush_size(paint_state_t *st, int size) {
    if (size < 1)  size = 1;
    if (size > 32) size = 32;
    st->brush_size = size;
    printf("size: %d%s\n", st->brush_size,
           st->tool == TOOL_PENCIL ? " (unused by pencil)" : "");
}

/* ---- mode callbacks ---- */

static void init(app_mode_t *m) {
    paint_state_t *st = calloc(1, sizeof *st);
    st->tool       = TOOL_PENCIL;
    st->brush_size = 5;
    st->color      = 0xFFFF8800;         /* default orange */
    st->last_cx    = -1;
    st->last_cy    = -1;

    /* Canvas at startup framebuffer size (retina-aware). Survives all
       window resizes; the window is just a viewport onto it. */
    platform_framebuffer_t *fb0 = platform_get_framebuffer();
    st->canvas_w = fb0->width;
    st->canvas_h = fb0->height;
    st->canvas   = malloc((size_t)st->canvas_w * (size_t)st->canvas_h * sizeof(u32));
    canvas_clear(st);

    m->state = st;
}

static void cleanup(app_mode_t *m) {
    paint_state_t *st = m->state;
    free(st->canvas);
    free(st);
    m->state = NULL;
}

static void leave(app_mode_t *m) {
    paint_state_t *st = m->state;
    st->painting    = false;          /* don't resume a stroke on re-enter */
    st->line_active = false;          /* uncommitted shapes are dropped */
    st->tri_n       = 0;
}

static void event(app_mode_t *m, const platform_event_t *e) {
    paint_state_t *st = m->state;
    switch (e->kind) {
    case PLATFORM_EV_KEY_DOWN:
        if (e->key.repeat) break;
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
            break;
        case PLATFORM_KEY_LEFT_BRACKET:  set_brush_size(st, st->brush_size - 1); break;
        case PLATFORM_KEY_RIGHT_BRACKET: set_brush_size(st, st->brush_size + 1); break;
        case PLATFORM_KEY_C:
            canvas_clear(st);
            printf("canvas cleared\n");
            break;
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
            if (st->tri_n == 3) triangle_commit(st);
            else { st->tri_x[st->tri_n] = cx; st->tri_y[st->tri_n] = cy; }  /* seed live corner */
            break;
        }

        if (!inside) break;
        if (st->tool == TOOL_LINE) {
            st->line_active = true;
            st->line_x0 = st->line_x1 = cx;
            st->line_y0 = st->line_y1 = cy;
            break;
        }
        st->painting = true;
        st->last_cx  = cx;
        st->last_cy  = cy;
        apply_tool_at(st, cx, cy);
    } break;

    case PLATFORM_EV_MOUSE_UP:
        if (e->mouse.btn != PLATFORM_MOUSE_LEFT) break;
        if (st->line_active) {
            mouse_to_canvas(st, e->mouse.x, e->mouse.y, &st->line_x1, &st->line_y1);
            line_commit(st);
        }
        st->painting = false;
        break;

    case PLATFORM_EV_MOUSE_MOVE: {
        if (tool_is_triangle(st->tool)) {
            if (st->tri_n > 0)
                mouse_to_canvas(st, e->move.x, e->move.y,
                                &st->tri_x[st->tri_n], &st->tri_y[st->tri_n]);
            break;
        }
        if (st->line_active) {
            mouse_to_canvas(st, e->move.x, e->move.y, &st->line_x1, &st->line_y1);
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
    if (st->line_active) render_line_preview(fb, st);
    if (st->tri_n)       render_triangle_preview(fb, st);
}

static void set_color(app_mode_t *m, u32 argb) {
    paint_state_t *st = m->state;
    st->color = argb;
    printf("paint color: 0x%08X\n", argb);
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
