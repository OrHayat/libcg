#include "platform/platform.h"
#include <stdio.h>

int main(void) {
    if (!platform_init(1280, 720, "libcg")) {
        fprintf(stderr, "Failed to initialize platform\n");
        return 1;
    }

    bool quit = false;
    while (!quit) {
        platform_poll_events(&quit);
        platform_present();
    }

    platform_shutdown();
    return 0;
}
