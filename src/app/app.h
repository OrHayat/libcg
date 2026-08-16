#ifndef APP_H
#define APP_H

#include "app/mode.h"

/* ============================================================
   App shell. Sits between platform_run and the modes: owns the mode
   list and the active index, handles keys that mean the same thing
   everywhere, and runs the '#'-hex color-input overlay on top of
   whichever mode is active.

   Global keys (consumed before the active mode sees them):
     Q          quit
     F          toggle fullscreen
     Esc        exit fullscreen (only when fullscreen)
     Tab        next mode
     F1..F12    switch to mode N directly
     M          toggle mouse-coordinate printing
     T / L      display info dumps
     #          begin color input; Enter applies to the active mode
                via mode->set_color, Esc cancels
   ============================================================ */

typedef struct {
    int         width;
    int         height;
    const char *title;
    bool        transparent;
    bool        resizable;
    bool        high_dpi;
} app_config_t;

/* Blocks until the window closes or Q is pressed. modes[0] is active at
   start. `modes` must outlive the call (main()'s stack is fine). Returns
   the process exit code. */
int app_run(app_mode_t *modes, int mode_count, const app_config_t *cfg);

#endif /* APP_H */
