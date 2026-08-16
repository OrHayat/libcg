#include "modes/paint.h"
#include "render/framebuffer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    TOOL_PENCIL = 0,    /* 1px dot at cursor, current color */
    TOOL_BRUSH,         /* brush_size × brush_size square, current color */
    TOOL_ERASER,        /* brush_size × brush_size square, white */
} paint_tool_t;

typedef struct {
    /* fixed-size canvas that survives window resizes */
    u32 *canvas;
    int  canvas_w, canvas_h;

    paint_tool_t tool;              /* default TOOL_PENCIL */
    int          brush_size;        /* 1..32, default 5 */
    u32          color;             /* straight ARGB from the shell's # input */
    bool         painting;          /* mouse held during a stroke */
    int          last_cx, last_cy;  /* prev stroke point in canvas coords; -1 = none */
} paint_state_t;

#define CANVAS_BG 0xFFFFFFFFu

/* ---- render: letterbox the canvas inside the framebuffer ----
   Window > canvas → gray bars; window < canvas → canvas pixels are
   clipped from view but remain in memory. No interpolation, ever.
   Invariant: this and mouse_to_canvas use the same offset formula
   so the cursor lands exactly on the painted pixel. */

static void render_canvas(platform_framebuffer_t *fb,
                          const u32 *canvas, int canvas_w, int canvas_h) {
    framebuffer_clear(fb, RGB(48, 48, 48));
    if (!canvas) return;

    int off_x = (fb->width  - canvas_w) / 2;
    int off_y = (fb->height - canvas_h) / 2;

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
    default:          return "?";
    }
}

/* Convert logical-point mouse coords to canvas-pixel coords. Mouse events
   arrive in logical points; the canvas is in framebuffer pixels, so scale
   by the DPI factor then subtract the letterbox offset. Returns false if
   the cursor is outside the canvas (in the gray bars). */
static bool mouse_to_canvas(const paint_state_t *st, int mouse_x, int mouse_y,
                            int *out_cx, int *out_cy) {
    /* Only width/height are read here — safe outside frame_cb. */
    platform_framebuffer_t *fb = platform_get_framebuffer();
    double scale = platform_get_dpi_scale();
    int fb_x = (int)(mouse_x * scale);
    int fb_y = (int)(mouse_y * scale);

    int off_x = (fb->width  - st->canvas_w) / 2;
    int off_y = (fb->height - st->canvas_h) / 2;
    int cx = fb_x - off_x;
    int cy = fb_y - off_y;

    if (cx < 0 || cx >= st->canvas_w) return false;
    if (cy < 0 || cy >= st->canvas_h) return false;
    *out_cx = cx;
    *out_cy = cy;
    return true;
}

/* Apply current tool centered at canvas-pixel (cx, cy). Out-of-bounds
   pixels in the brush footprint are clipped, not wrapped. */
static void apply_tool_at(paint_state_t *st, int cx, int cy) {
    u32 color;
    int radius;
    switch (st->tool) {
    case TOOL_PENCIL: color = st->color; radius = 0;                   break;
    case TOOL_BRUSH:  color = st->color; radius = st->brush_size / 2;  break;
    case TOOL_ERASER: color = CANVAS_BG; radius = st->brush_size / 2;  break;
    default: return;
    }

    int x0 = cx - radius, x1 = cx + radius;
    int y0 = cy - radius, y1 = cy + radius;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= st->canvas_w) x1 = st->canvas_w - 1;
    if (y1 >= st->canvas_h) y1 = st->canvas_h - 1;

    for (int y = y0; y <= y1; y++) {
        u32 *row = &st->canvas[y * st->canvas_w];
        for (int x = x0; x <= x1; x++) row[x] = color;
    }
}

/* Bresenham line — fills pixel gaps when the mouse moves faster than
   one event per pixel. Without this, fast strokes leave a string of
   dots instead of a continuous line. */
static void apply_tool_stroke(paint_state_t *st, int x0, int y0, int x1, int y1) {
    int dx =  abs(x1 - x0);
    int dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        apply_tool_at(st, x0, y0);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void canvas_clear(paint_state_t *st) {
    if (!st->canvas) return;
    size_t n = (size_t)st->canvas_w * (size_t)st->canvas_h;
    for (size_t i = 0; i < n; i++) st->canvas[i] = CANVAS_BG;
}

static void set_tool(paint_state_t *st, paint_tool_t t) {
    st->tool = t;
    if (t == TOOL_PENCIL) printf("tool: pencil\n");
    else                  printf("tool: %s (size %d)\n", tool_name(t), st->brush_size);
}

static void set_brush_size(paint_state_t *st, int size) {
    if (size < 1)  size = 1;
    if (size > 32) size = 32;
    st->brush_size = size;
    if (st->tool == TOOL_BRUSH || st->tool == TOOL_ERASER)
        printf("brush size: %d\n", st->brush_size);
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
    st->painting = false;             /* don't resume a stroke on re-enter */
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
        if (!mouse_to_canvas(st, e->mouse.x, e->mouse.y, &cx, &cy)) break;
        st->painting = true;
        st->last_cx  = cx;
        st->last_cy  = cy;
        apply_tool_at(st, cx, cy);
    } break;

    case PLATFORM_EV_MOUSE_UP:
        if (e->mouse.btn != PLATFORM_MOUSE_LEFT) break;
        st->painting = false;
        break;

    case PLATFORM_EV_MOUSE_MOVE: {
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
    render_canvas(fb, st->canvas, st->canvas_w, st->canvas_h);
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
