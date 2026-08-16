#ifndef MODE_PAINT_H
#define MODE_PAINT_H

#include "app/mode.h"

/* Fixed-size pixel canvas, letterboxed in the window (MS Paint model).
     1 pencil   2 brush   3 eraser      [ / ] brush size
     C clear    left-drag paints        # (shell) sets the paint color
   Canvas is allocated at the startup framebuffer size and survives
   window resizes. */
app_mode_t paint_mode(void);

#endif /* MODE_PAINT_H */
