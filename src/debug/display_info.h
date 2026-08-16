#ifndef DISPLAY_INFO_H
#define DISPLAY_INFO_H

/* stdout dumps of the platform display-enumeration API. Debug only —
   no state, safe to call from any callback. */

/* Framebuffer size + one block per connected display (window's marked *). */
void display_info_print_all(void);

/* Resolution/refresh table for the display the window is on. */
void display_info_print_window_modes(void);

#endif /* DISPLAY_INFO_H */
