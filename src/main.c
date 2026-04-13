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

    bool quit = false;
    while (!quit) {
        platform_poll_events(&quit);
        platform_present();
    }

    platform_shutdown();
    return 0;
}
