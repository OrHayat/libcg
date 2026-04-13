#include "platform/platform.h"
#include <stdio.h>

int main(void) {
    if (!platform_init(1280, 720, "libcg")) {
        fprintf(stderr, "Failed to initialize platform\n");
        return 1;
    }

    platform_framebuffer_t *fb = platform_get_framebuffer();
    for (int i = 0; i < fb->width * fb->height; i++) {
        fb->pixels[i] = 0xFFFF8800;  /* AARRGGBB - solid orange */
    }

    platform_input_t input = {0};
    while (!input.quit_requested) {
        platform_poll_events(&input);
        if (input.keys_pressed[PLATFORM_KEY_Q]) {
            input.quit_requested = true;
        }
        platform_present();
    }

    platform_shutdown();
    return 0;
}
