#ifndef COLOR_INPUT_H
#define COLOR_INPUT_H

#include "platform/platform.h"
#include "render/color.h"
#include "util/common.h"

/* '#'-triggered hex color entry on stdout. The caller detects the '#'
   TEXT_INPUT, calls color_input_begin, then routes every event to
   color_input_event while `active` is set. */

typedef struct {
    bool active;
    char buf[16];
    int  len;
} color_input_t;

void color_input_begin(color_input_t *ci);

/* Consumes one event. Returns true exactly when Enter completed a valid
   color, which is written to *out (straight, unpremultiplied). Enter and Esc both
   clear `active`. */
bool color_input_event(color_input_t *ci, const platform_event_t *e, color_t *out);

#endif /* COLOR_INPUT_H */
