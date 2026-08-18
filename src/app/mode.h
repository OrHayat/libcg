#ifndef MODE_H
#define MODE_H

#include "platform/platform.h"
#include "render/color.h"
#include "util/common.h"

/* ============================================================
   A mode is one self-contained interactive scene: it owns its state,
   receives the events the app shell doesn't consume, and paints the
   framebuffer each frame. Exactly one mode is active at a time; the
   shell (app.h) switches between them.

   Every callback is optional except frame. init runs once for every
   registered mode at startup (before the first frame, so
   platform_get_framebuffer() is valid); cleanup runs once at exit in
   reverse order. enter/leave bracket activation. set_color receives
   the result of the shell's shared '#'-hex color-input overlay.

   Modes must not keep file-scope mutable state — put it behind
   `state` (allocated in init, freed in cleanup) so two instances of
   the same mode could coexist.
   ============================================================ */

typedef struct app_mode app_mode_t;

struct app_mode {
    const char *name;                                       /* printed on switch */
    void       *state;                                      /* opaque, mode-owned */

    void (*init)     (app_mode_t *m);
    void (*cleanup)  (app_mode_t *m);
    void (*enter)    (app_mode_t *m);
    void (*leave)    (app_mode_t *m);
    void (*event)    (app_mode_t *m, const platform_event_t *e);
    void (*frame)    (app_mode_t *m, platform_framebuffer_t *fb);   /* required */
    void (*set_color)(app_mode_t *m, color_t c);                     /* straight, unpremultiplied */
};

#endif /* MODE_H */
