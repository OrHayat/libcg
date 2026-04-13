#include "platform/platform.h"
#include <stdio.h>

int main(void) {
    if (!platform_init(1280, 720, "libcg")) {
        fprintf(stderr, "Failed to initialize platform\n");
        return 1;
    }

    platform_framebuffer_t *fb = platform_get_framebuffer();
    for (int i = 0; i < fb->width * fb->height; i++) {
        fb->pixels[i] = 0xFFFF8800; /* AARRGGBB - solid orange */
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

        if (print_mouse_coords && (input.mouse_x != last_mx || input.mouse_y != last_my)) {
            printf("mouse: (%d, %d)\n", input.mouse_x, input.mouse_y);
            last_mx = input.mouse_x;
            last_my = input.mouse_y;
        }

        if (input.mouse_left_pressed)
            printf("mouse left down at (%d, %d)\n", input.mouse_x, input.mouse_y);
        if (input.mouse_right_pressed)
            printf("mouse right down at (%d, %d)\n", input.mouse_x, input.mouse_y);
        if (input.mouse_middle_pressed)
            printf("mouse middle down at (%d, %d)\n", input.mouse_x, input.mouse_y);
        if (input.scroll_dy != 0.0f)
            printf("scroll dy: %f\n", (double)input.scroll_dy);

        platform_present();
    }

    platform_shutdown();
    return 0;
}
