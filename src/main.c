#include "platform/platform.h"
#include <stdio.h>

int main(void) {
    if (!platform_init(1280, 720, "libcg")) {
        fprintf(stderr, "Failed to initialize platform\n");
        return 1;
    }

    bool print_mouse_coords = false;
    int  last_mx = -1, last_my = -1;

    platform_input_t input = {0};
    while (!input.quit_requested) {
        platform_poll_events(&input);

        if (platform_is_key_pressed(&input, PLATFORM_KEY_Q))
            input.quit_requested = true;
        if (platform_is_key_pressed(&input, PLATFORM_KEY_M)) {
            print_mouse_coords = !print_mouse_coords;
            printf("mouse coord printing: %s\n", print_mouse_coords ? "ON" : "OFF");
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
            printf("scroll: dx=%.1f dy=%.1f\n", (double)input.mouse.scroll_dx, (double)input.mouse.scroll_dy);

        platform_framebuffer_t *fb = platform_get_framebuffer();
        for (int i = 0; i < fb->width * fb->height; i++) {
            fb->pixels[i] = 0xFFFF8800;  /* AARRGGBB - solid orange */
        }

        platform_present();
    }

    platform_shutdown();
    return 0;
}
