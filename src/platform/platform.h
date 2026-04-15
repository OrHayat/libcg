#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdbool.h>
#include <stdint.h>

/* Lifecycle */
bool platform_init(int width, int height, const char *title);
void platform_shutdown(void);

/* Per-frame */
void platform_poll_events(bool *quit_requested);
void platform_present(void);

#endif /* PLATFORM_H */
