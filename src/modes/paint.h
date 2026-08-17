#ifndef MODE_PAINT_H
#define MODE_PAINT_H

#include "app/mode.h"

/* Fixed-size pixel canvas, letterboxed in the window (MS Paint model).
     1 pencil   2 brush   3 eraser   4 line
     5 triangle (filled)          6 triangle (outline)
     [ / ] brush size             C clear      # (shell) sets the color
   Pencil/brush/eraser paint on drag; line is press-drag-release;
   triangles are three clicks. Esc cancels a shape in progress.
   Canvas is allocated at the startup framebuffer size and survives
   window resizes. */
app_mode_t paint_mode(void);

#endif /* MODE_PAINT_H */
