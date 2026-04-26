#include "platform/platform.h"
#include "render/framebuffer.h"
#include "util/common.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

typedef enum {
    PATTERN_SOLID = 0,
    PATTERN_GRADIENT,
    PATTERN_CYCLE,
    PATTERN_NOISE,
    PATTERN_CUSTOM_COLOR,
} pattern_t;

static u32 s_custom_color = 0xFFFF8800;  /* default orange, 0xAARRGGBB unpremultiplied */

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

        /* find or add group keyed on pixel resolution */
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

        /* insert rate sorted desc, dedup */
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

    /* sort groups by pixel area ascending */
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

int main(void) {
    if (!platform_init(1280, 720, "libcg")) {
        fprintf(stderr, "Failed to initialize platform\n");
        return 1;
    }

    bool print_mouse_coords = false;
    bool bg_checkerboard    = false;
    int  last_mx = -1, last_my = -1;
    pattern_t pattern = PATTERN_SOLID;
    int  frame_count = 0;

    bool color_input_mode = false;
    char color_input_buf[16] = {0};
    int  color_input_len = 0;

    platform_input_t input = {0};
    while (!input.quit_requested) {
        platform_poll_events(&input);

        /* Process text input — may toggle color_input_mode */
        for (int i = 0; i < input.text_len; i++) {
            char c = input.text[i];
            if (!color_input_mode && c == '#') {
                color_input_mode = true;
                color_input_len  = 0;
                color_input_buf[0] = '\0';
                printf("color input mode (type hex digits, Enter to apply, Esc to cancel)\n");
                printf("color input: #");
                fflush(stdout);
            } else if (color_input_mode && c != '#' && hex_digit(c) >= 0 && color_input_len < 8) {
                color_input_buf[color_input_len++] = c;
                color_input_buf[color_input_len]   = '\0';
                printf("\r\x1b[Kcolor input: #%s", color_input_buf);
                fflush(stdout);
            }
        }

        if (color_input_mode) {
            if (platform_is_key_pressed(&input, PLATFORM_KEY_BACKSPACE) && color_input_len > 0) {
                color_input_buf[--color_input_len] = '\0';
                printf("\r\x1b[Kcolor input: #%s", color_input_buf);
                fflush(stdout);
            }
            if (platform_is_key_pressed(&input, PLATFORM_KEY_ENTER)) {
                u32 parsed;
                if (parse_hex_color(color_input_buf, color_input_len, &parsed)) {
                    s_custom_color = parsed;
                    pattern = PATTERN_CUSTOM_COLOR;
                    printf("\ncolor applied: #%s -> 0x%08X\n", color_input_buf, parsed);
                } else {
                    printf("\r\x1b[K#%s is not valid format\n", color_input_buf);
                }
                color_input_mode = false;
                color_input_len  = 0;
                color_input_buf[0] = '\0';
            }
            if (platform_is_key_pressed(&input, PLATFORM_KEY_ESCAPE)) {
                printf("\ncolor input cancelled\n");
                color_input_mode = false;
                color_input_len  = 0;
                color_input_buf[0] = '\0';
            }
        } else {
            if (platform_is_key_pressed(&input, PLATFORM_KEY_Q))
                input.quit_requested = true;
            if (platform_is_key_pressed(&input, PLATFORM_KEY_M)) {
                print_mouse_coords = !print_mouse_coords;
                printf("mouse coord printing: %s\n", print_mouse_coords ? "ON" : "OFF");
            }
            if (platform_is_key_pressed(&input, PLATFORM_KEY_F))
                platform_toggle_fullscreen();
            if (platform_is_key_pressed(&input, PLATFORM_KEY_ESCAPE) && platform_is_fullscreen())
                platform_toggle_fullscreen();
            if (platform_is_key_pressed(&input, PLATFORM_KEY_1)) pattern = PATTERN_SOLID;
            if (platform_is_key_pressed(&input, PLATFORM_KEY_2)) pattern = PATTERN_GRADIENT;
            if (platform_is_key_pressed(&input, PLATFORM_KEY_3)) pattern = PATTERN_CYCLE;
            if (platform_is_key_pressed(&input, PLATFORM_KEY_4)) pattern = PATTERN_NOISE;
            if (platform_is_key_pressed(&input, PLATFORM_KEY_B)) {
                bg_checkerboard = !bg_checkerboard;
                printf("background: %s\n", bg_checkerboard ? "checkerboard" : "transparent");
            }
            if (platform_is_key_pressed(&input, PLATFORM_KEY_T)) print_displays_with_framebuffer();
            if (platform_is_key_pressed(&input, PLATFORM_KEY_L)) print_window_display_modes();
        }

        /* Mouse output is suppressed while typing a color */
        if (!color_input_mode) {
            if (print_mouse_coords && (input.mouse.x != last_mx || input.mouse.y != last_my)) {
                printf("mouse: (%d, %d)\n", input.mouse.x, input.mouse.y);
                last_mx = input.mouse.x;
                last_my = input.mouse.y;
            }
            if (platform_is_mouse_pressed(&input, PLATFORM_MOUSE_LEFT))
                printf("mouse left pressed at (%d, %d)\n", input.mouse.x, input.mouse.y);
            if (platform_is_mouse_released(&input, PLATFORM_MOUSE_LEFT))
                printf("mouse left released at (%d, %d)\n", input.mouse.x, input.mouse.y);
            if (platform_is_mouse_pressed(&input, PLATFORM_MOUSE_RIGHT))
                printf("mouse right pressed at (%d, %d)\n", input.mouse.x, input.mouse.y);
            if (platform_is_mouse_released(&input, PLATFORM_MOUSE_RIGHT))
                printf("mouse right released at (%d, %d)\n", input.mouse.x, input.mouse.y);
            if (platform_is_mouse_pressed(&input, PLATFORM_MOUSE_MIDDLE))
                printf("mouse middle pressed at (%d, %d)\n", input.mouse.x, input.mouse.y);
            if (platform_is_mouse_released(&input, PLATFORM_MOUSE_MIDDLE))
                printf("mouse middle released at (%d, %d)\n", input.mouse.x, input.mouse.y);
            if (input.mouse.scroll_dx != 0.0f || input.mouse.scroll_dy != 0.0f)
                printf("scroll: dx=%.1f dy=%.1f\n", (double)input.mouse.scroll_dx, (double)input.mouse.scroll_dy);
        }

        platform_framebuffer_t *fb = platform_get_framebuffer();
        /* Background first — either checker or fully transparent.
           Required so alpha-blended patterns (CUSTOM_COLOR) have a
           correct base and don't pick up last frame's pixels. */
        if (bg_checkerboard) {
            render_checkerboard(fb);
        } else {
            framebuffer_clear(fb, 0x00000000);
        }
        switch (pattern) {
            case PATTERN_SOLID:        render_solid(fb); break;
            case PATTERN_GRADIENT:     render_gradient(fb); break;
            case PATTERN_CYCLE:        render_cycle(fb, frame_count * 0.05); break;
            case PATTERN_NOISE:        render_noise(fb); break;
            case PATTERN_CUSTOM_COLOR: render_custom_color(fb, s_custom_color); break;
        }

        frame_count++;
        platform_present();
    }

    platform_shutdown();
    return 0;
}
