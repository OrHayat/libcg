#ifndef MODE_PATTERN_H
#define MODE_PATTERN_H

#include "app/mode.h"

/* Debug fill patterns for the whole framebuffer.
     1 solid   2 gradient   3 color cycle   4 noise   5 draw2d primitives
     B toggle checkerboard background (vs. transparent window)
     # (shell) custom color, alpha-blended over the background
   Mouse buttons / scroll are echoed to stdout. */
app_mode_t pattern_mode(void);

#endif /* MODE_PATTERN_H */
