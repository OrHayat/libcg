#include "platform/platform.h"
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
} app_state_t;

/* ============================================================
   Pure helpers — no state.
   ============================================================ */

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_hex_color(const char *s, int len, u32 *out) {
    for (int i = 0; i < len; i++) {
        if (hex_digit(s[i]) < 0) return false;
    }
    if (len == 3) {
        u8 r = (u8)hex_digit(s[0]); r = (u8)((r << 4) | r);
        u8 g = (u8)hex_digit(s[1]); g = (u8)((g << 4) | g);
        u8 b = (u8)hex_digit(s[2]); b = (u8)((b << 4) | b);
        *out = (0xFFu << 24) | ((u32)r << 16) | ((u32)g << 8) | (u32)b;
        return true;
    } else if (len == 6) {
        u8 r = (u8)((hex_digit(s[0]) << 4) | hex_digit(s[1]));
        u8 g = (u8)((hex_digit(s[2]) << 4) | hex_digit(s[3]));
        u8 b = (u8)((hex_digit(s[4]) << 4) | hex_digit(s[5]));
        *out = (0xFFu << 24) | ((u32)r << 16) | ((u32)g << 8) | (u32)b;
        return true;
    } else if (len == 8) {
        u8 r = (u8)((hex_digit(s[0]) << 4) | hex_digit(s[1]));
        u8 g = (u8)((hex_digit(s[2]) << 4) | hex_digit(s[3]));
        u8 b = (u8)((hex_digit(s[4]) << 4) | hex_digit(s[5]));
        u8 a = (u8)((hex_digit(s[6]) << 4) | hex_digit(s[7]));
        *out = ((u32)a << 24) | ((u32)r << 16) | ((u32)g << 8) | (u32)b;
        return true;
    }
    return false;
}

static u32 premultiply(u32 argb) {
    u8 a = (u8)((argb >> 24) & 0xFF);
    u8 r = (u8)((argb >> 16) & 0xFF);
    u8 g = (u8)((argb >>  8) & 0xFF);
    u8 b = (u8)( argb        & 0xFF);
    r = (u8)((r * a) / 255);
    g = (u8)((g * a) / 255);
    b = (u8)((b * a) / 255);
    return ((u32)a << 24) | ((u32)r << 16) | ((u32)g << 8) | (u32)b;
}

static u32 alpha_blend(u32 dst, u32 src) {
    /* both premultiplied; result = src + dst * (1 - src_alpha) */
    u8 sa = (u8)((src >> 24) & 0xFF);
    u8 sr = (u8)((src >> 16) & 0xFF);
    u8 sg = (u8)((src >>  8) & 0xFF);
    u8 sb = (u8)( src        & 0xFF);
    u8 da = (u8)((dst >> 24) & 0xFF);
    u8 dr = (u8)((dst >> 16) & 0xFF);
    u8 dg = (u8)((dst >>  8) & 0xFF);
    u8 db = (u8)( dst        & 0xFF);
    u8 inv = (u8)(255 - sa);
    u8 r = (u8)(sr + (dr * inv) / 255);
    u8 g = (u8)(sg + (dg * inv) / 255);
    u8 b = (u8)(sb + (db * inv) / 255);
    u8 a = (u8)(sa + (da * inv) / 255);
    return ((u32)a << 24) | ((u32)r << 16) | ((u32)g << 8) | (u32)b;
}

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
    u32 src = premultiply(argb);
    int n = fb->width * fb->height;
    for (int i = 0; i < n; i++) {
        fb->pixels[i] = alpha_blend(fb->pixels[i], src);
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
   Display-info pretty-printers — read platform accessors directly,
   no app state.
   ============================================================ */

static const char *connection_type_str(platform_connection_type_t t) {
    switch (t) {
        case PLATFORM_CONNECTION_INTERNAL:    return "Internal";
        case PLATFORM_CONNECTION_HDMI:        return "HDMI";
        case PLATFORM_CONNECTION_DISPLAYPORT: return "DisplayPort";
        case PLATFORM_CONNECTION_THUNDERBOLT: return "Thunderbolt";
        case PLATFORM_CONNECTION_AIRPLAY:     return "AirPlay";
        case PLATFORM_CONNECTION_VGA:         return "VGA";
        case PLATFORM_CONNECTION_DVI:         return "DVI";
        default:                              return "Unknown";
    }
}

static void print_display(int idx, const platform_display_info_t *d, bool is_window) {
    printf("  [%d]%s %-32s  id=%u scale=%.1f main=%d builtin=%d online=%d\n",
           idx, is_window ? " *" : "  ", d->name,
           d->id, (double)d->scale, d->is_main, d->builtin, d->is_online);
    if (d->name_original[0])
        printf("       original=\"%s\"\n", d->name_original);
    printf("       bounds=(%d,%d)+(%dx%dpt)  work=(%d,%d)+(%dx%dpt)  pixels=%dx%d  size=%dx%dmm\n",
           d->bounds_x, d->bounds_y, d->bounds_w, d->bounds_h,
           d->work_x, d->work_y, d->work_w, d->work_h,
           d->pixels_w, d->pixels_h, d->size_mm_w, d->size_mm_h);
    printf("       refresh=%.2fHz  rotation=%d°%s  conn=%s%s\n",
           (double)d->refresh_hz, d->rotation,
           d->rotation_supported ? " (rotatable)" : "",
           connection_type_str(d->connection_type),
           d->mirrors_id ? "  (mirrors another)" : "");
}

/* One row per distinct pixel resolution (collapses HiDPI variants),
   integer refresh rates ≥ 60Hz only (drops video rates like 47.95/48/50/59.94),
   sorted by area ascending. Shaped like a game's resolution dropdown. */
static void print_window_display_modes(void) {
    typedef struct {
        int  pw, ph;
        int  rates[8];     /* unique integer rates, sorted desc */
        int  rate_count;
        bool has_current;
        int  current_rate;
    } group_t;

    uint32_t my = platform_get_window_display_id();
    platform_video_mode_t modes[128];
    int n = platform_get_display_modes(my, modes, 128);

    group_t groups[16];
    int gc = 0;
    for (int i = 0; i < n; i++) {
        int rate = (int)(modes[i].refresh_hz + 0.5f);
        if (rate < 60) continue;  /* drop 24/30/48/50/etc — video cadences */

        int gi = -1;
        for (int j = 0; j < gc; j++) {
            if (groups[j].pw == modes[i].pixels_w && groups[j].ph == modes[i].pixels_h) {
                gi = j; break;
            }
        }
        if (gi < 0 && gc < 16) {
            gi = gc++;
            groups[gi].pw = modes[i].pixels_w;
            groups[gi].ph = modes[i].pixels_h;
            groups[gi].rate_count = 0;
            groups[gi].has_current = false;
        }
        if (gi < 0) continue;

        group_t *g = &groups[gi];
        bool seen = false;
        for (int r = 0; r < g->rate_count; r++) {
            if (g->rates[r] == rate) { seen = true; break; }
        }
        if (!seen && g->rate_count < 8) {
            int pos = g->rate_count;
            while (pos > 0 && g->rates[pos-1] < rate) { g->rates[pos] = g->rates[pos-1]; pos--; }
            g->rates[pos] = rate;
            g->rate_count++;
        }
        if (modes[i].is_current) {
            g->has_current = true;
            g->current_rate = rate;
        }
    }

    for (int i = 1; i < gc; i++) {
        group_t tmp = groups[i];
        int j = i;
        while (j > 0 && (long long)groups[j-1].pw * groups[j-1].ph > (long long)tmp.pw * tmp.ph) {
            groups[j] = groups[j-1]; j--;
        }
        groups[j] = tmp;
    }

    printf("modes for display id=%u: %d total, %d resolutions\n", my, n, gc);
    for (int i = 0; i < gc; i++) {
        group_t *g = &groups[i];
        char rates[64] = {0};
        size_t off = 0;
        for (int r = 0; r < g->rate_count; r++) {
            int wrote = snprintf(rates + off, sizeof(rates) - off,
                                 "%s%d", r == 0 ? "" : "/", g->rates[r]);
            if (wrote > 0 && (size_t)wrote < sizeof(rates) - off) off += (size_t)wrote;
        }
        if (g->has_current)
            printf("  %4dx%-4d  %sHz  ← current @ %dHz\n", g->pw, g->ph, rates, g->current_rate);
        else
            printf("  %4dx%-4d  %sHz\n", g->pw, g->ph, rates);
    }
}

static void print_displays_with_framebuffer(void) {
    platform_framebuffer_t *fb = platform_get_framebuffer();
    printf("framebuffer: %dx%d\n", fb->width, fb->height);

    platform_display_info_t displays[8];
    int n = platform_get_displays(displays, 8);
    uint32_t my = platform_get_window_display_id();
    printf("displays: %d\n", n);
    for (int i = 0; i < n; i++) {
        print_display(i, &displays[i], displays[i].id == my);
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
            if (parse_hex_color(app->color_input_buf, app->color_input_len, &parsed)) {
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
        if (c != '#' && hex_digit(c) >= 0 && app->color_input_len < 8) {
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
        case PLATFORM_KEY_T: print_displays_with_framebuffer(); break;
        case PLATFORM_KEY_L: print_window_display_modes();      break;
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
   Paint mode.
   ============================================================ */

static void paint_on_event(app_state_t *app, const platform_event_t *e) {
    switch (e->kind) {
    case PLATFORM_EV_KEY_DOWN:
        if (e->key.repeat) break;
        switch (e->key.key) {
        case PLATFORM_KEY_M:
            app->print_mouse_coords = !app->print_mouse_coords;
            printf("mouse coord printing: %s\n", app->print_mouse_coords ? "ON" : "OFF");
            break;
        case PLATFORM_KEY_T: print_displays_with_framebuffer(); break;
        case PLATFORM_KEY_L: print_window_display_modes();      break;
        case PLATFORM_KEY_P:
            app->mode = MODE_PATTERN;
            printf("mode: pattern\n");
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

    /* Mouse position tracking happens regardless of mode/overlay. */
    if (e->kind == PLATFORM_EV_MOUSE_MOVE) {
        app->mouse_x = e->move.x;
        app->mouse_y = e->move.y;
        if (!app->color_input_active && app->print_mouse_coords)
            printf("mouse: (%d, %d)\n", app->mouse_x, app->mouse_y);
        return;
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
