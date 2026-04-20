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
} pattern_t;

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
    bool bg_checkerboard = false;
    int last_mx = -1, last_my = -1;
    pattern_t pattern = PATTERN_SOLID;
    int frame_count = 0;

    platform_input_t input = {0};
    while (!input.quit_requested) {
        platform_poll_events(&input);

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
        if (platform_is_key_pressed(&input, PLATFORM_KEY_1))
            pattern = PATTERN_SOLID;
        if (platform_is_key_pressed(&input, PLATFORM_KEY_2))
            pattern = PATTERN_GRADIENT;
        if (platform_is_key_pressed(&input, PLATFORM_KEY_3))
            pattern = PATTERN_CYCLE;
        if (platform_is_key_pressed(&input, PLATFORM_KEY_4))
            pattern = PATTERN_NOISE;
        if (platform_is_key_pressed(&input, PLATFORM_KEY_B)) {
            bg_checkerboard = !bg_checkerboard;
            printf("background: %s\n", bg_checkerboard ? "checkerboard" : "transparent");
        }

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
            printf("scroll: dx=%.1f dy=%.1f\n", (double)input.mouse.scroll_dx,
                   (double)input.mouse.scroll_dy);

        platform_framebuffer_t *fb = platform_get_framebuffer();
        if (bg_checkerboard)
            render_checkerboard(fb);
        switch (pattern) {
        case PATTERN_SOLID:
            render_solid(fb);
            break;
        case PATTERN_GRADIENT:
            render_gradient(fb);
            break;
        case PATTERN_CYCLE:
            render_cycle(fb, frame_count * 0.05);
            break;
        case PATTERN_NOISE:
            render_noise(fb);
            break;
        }

        frame_count++;
        platform_present();
    }

    platform_shutdown();
    return 0;
}
