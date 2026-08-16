#include "debug/display_info.h"
#include "platform/platform.h"
#include "render/color.h"
#include "render/framebuffer.h"
#include "util/common.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================
   App state — passed as desc.user_data, threaded through every
   callback. Lives in main()'s stack frame; main() blocks on
   platform_run so the pointer is valid for the whole app lifetime.
   No file-scope mutable globals.
   ============================================================ */

typedef enum {
    PATTERN_SOLID = 0,
    PATTERN_GRADIENT,
    PATTERN_CYCLE,
    PATTERN_NOISE,
    PATTERN_CUSTOM_COLOR,
} pattern_t;

typedef enum {
    MODE_PATTERN,
    MODE_PAINT,
} app_mode_t;

typedef enum {
    TOOL_PENCIL = 0,    /* 1px dot at cursor, current color */
    TOOL_BRUSH,         /* brush_size × brush_size square, current color */
    TOOL_ERASER,        /* brush_size × brush_size square, white */
} paint_tool_t;

typedef struct {
    /* main mode */
    app_mode_t mode;

    /* color-input overlay — orthogonal to main mode; renders below */
    bool color_input_active;
    char color_input_buf[16];
    int  color_input_len;

    /* pattern mode state (persists when switching to paint and back) */
    pattern_t pattern;
    u32       custom_color;          /* 0xAARRGGBB unpremultiplied */
    bool      bg_checkerboard;

    /* debug toggles */
    bool print_mouse_coords;

    /* mouse position tracked from PLATFORM_EV_MOUSE_MOVE */
    int mouse_x, mouse_y;

    /* paint mode — fixed-size canvas that survives window resizes */
    u32 *paint_canvas;
    int  paint_canvas_w, paint_canvas_h;

    /* paint tool state */
    paint_tool_t paint_tool;          /* current tool, default TOOL_PENCIL */
    int          brush_size;          /* size for brush/eraser, 1..32, default 5 */
    bool         painting;            /* mouse held during a stroke */
    int          last_paint_cx;       /* prev stroke point in canvas coords */
    int          last_paint_cy;
} app_state_t;

/* ============================================================
   Pure helpers — no state.
   ============================================================ */

static const char *mouse_button_name(platform_mouse_button_t b) {
    switch (b) {
    case PLATFORM_MOUSE_LEFT:   return "left";
    case PLATFORM_MOUSE_RIGHT:  return "right";
    case PLATFORM_MOUSE_MIDDLE: return "middle";
    default:                    return "?";
    }
}

/* ============================================================
   Render helpers — take fb (and any extra args), no app state.
   ============================================================ */

static void render_custom_color(platform_framebuffer_t *fb, u32 argb) {
    u32 src = color_premultiply(argb);
    int n = fb->width * fb->height;
    for (int i = 0; i < n; i++) {
        fb->pixels[i] = color_blend(fb->pixels[i], src);
    }
}

static void render_solid(platform_framebuffer_t *fb) {
    framebuffer_clear(fb, RGB(0xFF, 0x88, 0x00));
}

static void render_gradient(platform_framebuffer_t *fb) {
    int w = fb->width > 1 ? fb->width - 1 : 1;
    for (int y = 0; y < fb->height; y++) {
        for (int x = 0; x < fb->width; x++) {
            u8 r = (u8)((x * 255) / w);
            fb->pixels[y * fb->width + x] = RGB(r, 0, 255 - r);
        }
    }
}

static void render_cycle(platform_framebuffer_t *fb, double t) {
    u8 r = (u8)(128.0 + 127.0 * sin(t));
    u8 g = (u8)(128.0 + 127.0 * sin(t + 2.094));
    u8 b = (u8)(128.0 + 127.0 * sin(t + 4.188));
    framebuffer_clear(fb, RGB(r, g, b));
}

static void render_noise(platform_framebuffer_t *fb) {
    int n = fb->width * fb->height;
    for (int i = 0; i < n; i++) {
        u8 v = (u8)(rand() & 0xFF);
        fb->pixels[i] = RGB(v, v, v);
    }
}

static void render_checkerboard(platform_framebuffer_t *fb) {
    int cell = 32;
    for (int y = 0; y < fb->height; y++) {
        for (int x = 0; x < fb->width; x++) {
            bool light = (((x / cell) + (y / cell)) & 1) == 0;
            fb->pixels[y * fb->width + x] = light ? RGB(220, 220, 220) : RGB(160, 160, 160);
        }
    }
}

/* Letterbox the canvas inside the framebuffer (Photoshop / MS Paint model).
   Window > canvas → gray bars; window < canvas → canvas pixels are clipped
   from view but remain in memory. No interpolation, ever. */
static void render_paint_mode(platform_framebuffer_t *fb,
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

/* ============================================================
   Color-input overlay. Active independently of the main mode;
   text input goes here, the main mode renders below.
   ============================================================ */

static void color_input_enter(app_state_t *app) {
    app->color_input_active = true;
    app->color_input_len    = 0;
    app->color_input_buf[0] = '\0';
    printf("color input mode (type hex digits, Enter to apply, Esc to cancel)\n");
    printf("color input: #");
    fflush(stdout);
}

static void color_input_exit(app_state_t *app) {
    app->color_input_active = false;
    app->color_input_len    = 0;
    app->color_input_buf[0] = '\0';
}

static void color_input_on_event(app_state_t *app, const platform_event_t *e) {
    switch (e->kind) {
    case PLATFORM_EV_KEY_DOWN:
        if (e->key.key == PLATFORM_KEY_BACKSPACE && app->color_input_len > 0) {
            app->color_input_buf[--app->color_input_len] = '\0';
            printf("\r\x1b[Kcolor input: #%s", app->color_input_buf);
            fflush(stdout);
        } else if (e->key.key == PLATFORM_KEY_ENTER && !e->key.repeat) {
            u32 parsed;
            if (color_parse_hex(app->color_input_buf, app->color_input_len, &parsed)) {
                app->custom_color = parsed;
                app->pattern      = PATTERN_CUSTOM_COLOR;
                printf("\ncolor applied: #%s -> 0x%08X\n", app->color_input_buf, parsed);
            } else {
                printf("\r\x1b[K#%s is not valid format\n", app->color_input_buf);
            }
            color_input_exit(app);
        } else if (e->key.key == PLATFORM_KEY_ESCAPE && !e->key.repeat) {
            printf("\ncolor input cancelled\n");
            color_input_exit(app);
        }
        break;
    case PLATFORM_EV_TEXT_INPUT: {
        char c = e->text.ch[0];
        if (c != '#' && color_hex_digit(c) >= 0 && app->color_input_len < 8) {
            app->color_input_buf[app->color_input_len++] = c;
            app->color_input_buf[app->color_input_len]   = '\0';
            printf("\r\x1b[Kcolor input: #%s", app->color_input_buf);
            fflush(stdout);
        }
    } break;
    default:
        break;
    }
}

/* ============================================================
   Pattern mode.
   ============================================================ */

static void pattern_on_event(app_state_t *app, const platform_event_t *e) {
    switch (e->kind) {
    case PLATFORM_EV_KEY_DOWN:
        if (e->key.repeat) break;
        switch (e->key.key) {
        case PLATFORM_KEY_M:
            app->print_mouse_coords = !app->print_mouse_coords;
            printf("mouse coord printing: %s\n", app->print_mouse_coords ? "ON" : "OFF");
            break;
        case PLATFORM_KEY_1: app->pattern = PATTERN_SOLID;    break;
        case PLATFORM_KEY_2: app->pattern = PATTERN_GRADIENT; break;
        case PLATFORM_KEY_3: app->pattern = PATTERN_CYCLE;    break;
        case PLATFORM_KEY_4: app->pattern = PATTERN_NOISE;    break;
        case PLATFORM_KEY_B:
            app->bg_checkerboard = !app->bg_checkerboard;
            printf("background: %s\n", app->bg_checkerboard ? "checkerboard" : "transparent");
            break;
        case PLATFORM_KEY_T: display_info_print_all(); break;
        case PLATFORM_KEY_L: display_info_print_window_modes();      break;
        case PLATFORM_KEY_P:
            app->mode = MODE_PAINT;
            printf("mode: paint\n");
            break;
        default: break;
        }
        break;
    case PLATFORM_EV_MOUSE_DOWN:
        printf("mouse %s pressed at (%d, %d)\n",
               mouse_button_name(e->mouse.btn), e->mouse.x, e->mouse.y);
        break;
    case PLATFORM_EV_MOUSE_UP:
        printf("mouse %s released at (%d, %d)\n",
               mouse_button_name(e->mouse.btn), e->mouse.x, e->mouse.y);
        break;
    case PLATFORM_EV_SCROLL:
        if (e->scroll.dx != 0.0f || e->scroll.dy != 0.0f)
            printf("scroll: dx=%.1f dy=%.1f\n",
                   (double)e->scroll.dx, (double)e->scroll.dy);
        break;
    default:
        break;
    }
}

static void pattern_on_frame(app_state_t *app, platform_framebuffer_t *fb) {
    /* Background first — either checker or fully transparent.
       Required so alpha-blended patterns (CUSTOM_COLOR) have a correct
       base and don't pick up last frame's pixels. */
    if (app->bg_checkerboard) render_checkerboard(fb);
    else                      framebuffer_clear(fb, 0x00000000);

    switch (app->pattern) {
    case PATTERN_SOLID:        render_solid(fb);                          break;
    case PATTERN_GRADIENT:     render_gradient(fb);                       break;
    case PATTERN_CYCLE:        render_cycle(fb, platform_now() * 3.0);    break;
    case PATTERN_NOISE:        render_noise(fb);                          break;
    case PATTERN_CUSTOM_COLOR: render_custom_color(fb, app->custom_color); break;
    }
}

/* ============================================================
   Paint mode — pencil / brush / eraser, brush sizing, clear canvas.
   Mouse coords from events are in logical points; canvas is in
   framebuffer pixels. Conversion: multiply by platform_get_dpi_scale()
   to get fb-pixel coords, then subtract the letterbox offset that
   render_paint_mode applies. Invariant: render_paint_mode and
   mouse_to_canvas use the same offset formula so cursor lines up
   exactly with painted pixels.
   ============================================================ */

static const char *tool_name(paint_tool_t t) {
    switch (t) {
    case TOOL_PENCIL: return "pencil";
    case TOOL_BRUSH:  return "brush";
    case TOOL_ERASER: return "eraser";
    default:          return "?";
    }
}

/* Convert logical-point mouse coords to canvas-pixel coords.
   Returns false if the cursor is outside the canvas (in the gray
   letterbox bars). */
static bool mouse_to_canvas(app_state_t *app, int mouse_x, int mouse_y,
                            int *out_cx, int *out_cy) {
    platform_framebuffer_t *fb = platform_get_framebuffer();
    double scale = platform_get_dpi_scale();
    int fb_x = (int)(mouse_x * scale);
    int fb_y = (int)(mouse_y * scale);

    int off_x = (fb->width  - app->paint_canvas_w) / 2;
    int off_y = (fb->height - app->paint_canvas_h) / 2;
    int cx = fb_x - off_x;
    int cy = fb_y - off_y;

    if (cx < 0 || cx >= app->paint_canvas_w) return false;
    if (cy < 0 || cy >= app->paint_canvas_h) return false;
    *out_cx = cx;
    *out_cy = cy;
    return true;
}

/* Apply current tool centered at canvas-pixel (cx, cy). Out-of-bounds
   pixels in the brush footprint are clipped, not wrapped. */
static void apply_tool_at(app_state_t *app, int cx, int cy) {
    u32 color;
    int radius;
    switch (app->paint_tool) {
    case TOOL_PENCIL: color = app->custom_color; radius = 0;                       break;
    case TOOL_BRUSH:  color = app->custom_color; radius = app->brush_size / 2;     break;
    case TOOL_ERASER: color = 0xFFFFFFFFu;       radius = app->brush_size / 2;     break;
    default: return;
    }

    int x0 = cx - radius, x1 = cx + radius;
    int y0 = cy - radius, y1 = cy + radius;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= app->paint_canvas_w) x1 = app->paint_canvas_w - 1;
    if (y1 >= app->paint_canvas_h) y1 = app->paint_canvas_h - 1;

    for (int y = y0; y <= y1; y++) {
        u32 *row = &app->paint_canvas[y * app->paint_canvas_w];
        for (int x = x0; x <= x1; x++) row[x] = color;
    }
}

/* Bresenham line — fills pixel gaps when the mouse moves faster than
   one event per pixel. Without this, fast strokes leave a string of
   dots instead of a continuous line. */
static void apply_tool_stroke(app_state_t *app, int x0, int y0, int x1, int y1) {
    int dx =  abs(x1 - x0);
    int dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        apply_tool_at(app, x0, y0);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void paint_canvas_clear(app_state_t *app) {
    if (!app->paint_canvas) return;
    size_t n = (size_t)app->paint_canvas_w * (size_t)app->paint_canvas_h;
    for (size_t i = 0; i < n; i++) app->paint_canvas[i] = 0xFFFFFFFFu;
    printf("canvas cleared\n");
}

static void paint_set_tool(app_state_t *app, paint_tool_t t) {
    app->paint_tool = t;
    if (t == TOOL_PENCIL) printf("tool: pencil\n");
    else                  printf("tool: %s (size %d)\n", tool_name(t), app->brush_size);
}

static void paint_set_brush_size(app_state_t *app, int size) {
    if (size < 1)  size = 1;
    if (size > 32) size = 32;
    app->brush_size = size;
    if (app->paint_tool == TOOL_BRUSH || app->paint_tool == TOOL_ERASER)
        printf("brush size: %d\n", app->brush_size);
}

static void paint_on_event(app_state_t *app, const platform_event_t *e) {
    switch (e->kind) {
    case PLATFORM_EV_KEY_DOWN:
        if (e->key.repeat) break;
        switch (e->key.key) {
        case PLATFORM_KEY_M:
            app->print_mouse_coords = !app->print_mouse_coords;
            printf("mouse coord printing: %s\n", app->print_mouse_coords ? "ON" : "OFF");
            break;
        case PLATFORM_KEY_T: display_info_print_all(); break;
        case PLATFORM_KEY_L: display_info_print_window_modes();      break;
        case PLATFORM_KEY_P:
            app->mode = MODE_PATTERN;
            printf("mode: pattern\n");
            break;

        /* Tool selection — positional, room to grow as more tools land. */
        case PLATFORM_KEY_1: paint_set_tool(app, TOOL_PENCIL); break;
        case PLATFORM_KEY_2: paint_set_tool(app, TOOL_BRUSH);  break;
        case PLATFORM_KEY_3: paint_set_tool(app, TOOL_ERASER); break;

        /* Brush size: ±1 step. */
        case PLATFORM_KEY_LEFT_BRACKET:  paint_set_brush_size(app, app->brush_size - 1); break;
        case PLATFORM_KEY_RIGHT_BRACKET: paint_set_brush_size(app, app->brush_size + 1); break;

        case PLATFORM_KEY_C: paint_canvas_clear(app); break;

        default: break;
        }
        break;

    case PLATFORM_EV_MOUSE_DOWN: {
        if (e->mouse.btn != PLATFORM_MOUSE_LEFT) break;
        int cx, cy;
        if (!mouse_to_canvas(app, e->mouse.x, e->mouse.y, &cx, &cy)) break;
        app->painting      = true;
        app->last_paint_cx = cx;
        app->last_paint_cy = cy;
        apply_tool_at(app, cx, cy);
    } break;

    case PLATFORM_EV_MOUSE_UP:
        if (e->mouse.btn != PLATFORM_MOUSE_LEFT) break;
        app->painting = false;
        break;

    case PLATFORM_EV_MOUSE_MOVE: {
        if (!app->painting) break;
        int cx, cy;
        if (!mouse_to_canvas(app, e->move.x, e->move.y, &cx, &cy)) {
            /* Cursor left the canvas mid-stroke — drop the segment but
               don't end the stroke (re-entering the canvas continues
               from the new position). Reset last_paint to the new spot
               on re-entry to avoid drawing a long line across the gap. */
            app->last_paint_cx = -1;
            app->last_paint_cy = -1;
            break;
        }
        if (app->last_paint_cx < 0) {
            apply_tool_at(app, cx, cy);
        } else {
            apply_tool_stroke(app, app->last_paint_cx, app->last_paint_cy, cx, cy);
        }
        app->last_paint_cx = cx;
        app->last_paint_cy = cy;
    } break;

    default:
        break;
    }
}

static void paint_on_frame(app_state_t *app, platform_framebuffer_t *fb) {
    render_paint_mode(fb, app->paint_canvas, app->paint_canvas_w, app->paint_canvas_h);
}

/* ============================================================
   Top-level callbacks. Dispatch to mode-specific handlers.
   ============================================================ */

/* Hotkeys that bypass mode and overlay — Q quits, F toggles fullscreen,
   Esc exits fullscreen. Returns true if handled (caller should stop). */
static bool handle_global_hotkey(const platform_event_t *e) {
    if (e->kind != PLATFORM_EV_KEY_DOWN || e->key.repeat) return false;
    switch (e->key.key) {
    case PLATFORM_KEY_Q: platform_request_quit();      return true;
    case PLATFORM_KEY_F: platform_toggle_fullscreen(); return true;
    case PLATFORM_KEY_ESCAPE:
        if (platform_is_fullscreen()) { platform_toggle_fullscreen(); return true; }
        return false;
    default:
        return false;
    }
}

static void on_init(void *ud) {
    app_state_t *app = ud;

    /* Allocate paint canvas at startup framebuffer size (retina-aware).
       Survives all window resizes; window is just a viewport onto it. */
    platform_framebuffer_t *fb0 = platform_get_framebuffer();
    app->paint_canvas_w = fb0->width;
    app->paint_canvas_h = fb0->height;

    size_t pixel_count = (size_t)app->paint_canvas_w * (size_t)app->paint_canvas_h;
    app->paint_canvas = malloc(pixel_count * sizeof(u32));
    if (app->paint_canvas) {
        for (size_t i = 0; i < pixel_count; i++) app->paint_canvas[i] = 0xFFFFFFFFu;
    }
}

static void on_cleanup(void *ud) {
    app_state_t *app = ud;
    free(app->paint_canvas);
    app->paint_canvas = NULL;
}

static void on_event(const platform_event_t *e, void *ud) {
    app_state_t *app = ud;

    /* Mouse position tracking — update the cached position regardless of
       mode/overlay, then fall through so mode handlers can react to the
       move (e.g. paint mode extends a stroke). */
    if (e->kind == PLATFORM_EV_MOUSE_MOVE) {
        app->mouse_x = e->move.x;
        app->mouse_y = e->move.y;
        if (!app->color_input_active && app->print_mouse_coords)
            printf("mouse: (%d, %d)\n", app->mouse_x, app->mouse_y);
    }

    if (handle_global_hotkey(e)) return;

    /* Color-input overlay: text input goes to the buffer; the mode below
       still renders. While active, mouse + non-Esc/Enter/Backspace keys
       are ignored (matches the original behavior). */
    if (app->color_input_active) {
        color_input_on_event(app, e);
        return;
    }
    if (e->kind == PLATFORM_EV_TEXT_INPUT && e->text.ch[0] == '#') {
        color_input_enter(app);
        return;
    }

    switch (app->mode) {
    case MODE_PATTERN: pattern_on_event(app, e); break;
    case MODE_PAINT:   paint_on_event(app, e);   break;
    }
}

static void on_frame(void *ud) {
    app_state_t *app = ud;
    platform_framebuffer_t *fb = platform_get_framebuffer();
    /* color-input overlay has no frame of its own — main mode renders below. */
    switch (app->mode) {
    case MODE_PATTERN: pattern_on_frame(app, fb); break;
    case MODE_PAINT:   paint_on_frame(app, fb);   break;
    }
}

int main(void) {
    app_state_t app = {
        .mode               = MODE_PATTERN,
        .pattern            = PATTERN_SOLID,
        .custom_color       = 0xFFFF8800,           /* default orange */
        .bg_checkerboard    = false,
        .print_mouse_coords = false,
        .color_input_active = false,
        .paint_tool         = TOOL_PENCIL,
        .brush_size         = 5,
        .painting           = false,
        .last_paint_cx      = -1,
        .last_paint_cy      = -1,
        /* paint_canvas allocated in on_init from current backing size */
    };

    return platform_run(&(platform_app_desc_t){
        .width       = 1280,
        .height      = 720,
        .title       = "libcg",
        .transparent = true,
        .resizable   = true,
        .high_dpi    = true,
        .user_data   = &app,
        .init_cb     = on_init,
        .frame_cb    = on_frame,
        .event_cb    = on_event,
        .cleanup_cb  = on_cleanup,
    });
}
