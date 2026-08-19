#ifndef UI_FONT_H
#define UI_FONT_H

#include "platform/platform.h"
#include "render/color.h"

/* Bitmap text for the UI layer. The 3D pipeline has no text of its own,
   so this is also what any later debug overlay draws with.

   Cell is 8x8: a 5x7 face plus a blank row that the descenders of
   g j p q y drop into. Advance is 6, leaving one column between glyphs. */
#define UI_FONT_CELL_H  8    /* cell height, and the line-to-line advance */
#define UI_FONT_ADVANCE 6    /* 5px face + 1px gap */

/* Draws `s` with its top-left at (x, y) and returns the x advance.
   Blends rather than overwrites, so translucent text over a panel works.
   `scale` is an integer pixel multiplier (2 on a retina backing store) —
   deliberately unfiltered, since a doubled bitmap font should stay crisp
   rather than turn to mush. '\n' returns to x and drops one line.
   Clipped per pixel to fb. */
int ui_font_draw(platform_framebuffer_t *fb, int x, int y, const char *s,
                 int scale, pcolor_t color);

/* Width the same string would occupy — the widest line, if multi-line. */
int ui_font_width(const char *s, int scale);

#endif /* UI_FONT_H */
