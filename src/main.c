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
            if (platform_is_key_pressed(&input, PLATFORM_KEY_T)) {
                platform_framebuffer_t *fbi = platform_get_framebuffer();
                printf("framebuffer: %dx%d\n", fbi->width, fbi->height);
            }
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
