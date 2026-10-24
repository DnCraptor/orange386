#ifndef ORANGE386_FTPUI_H
#define ORANGE386_FTPUI_H

#include <stdbool.h>

/*
 * Win+F10: FTP access to the FAT volumes of the attached disk images.
 *
 * Linux counterpart of murm386's USB DEVICE mode.  Opening the page pauses
 * the guest and starts the FTP server (ftpd.c); Esc stops the server and
 * restarts the emulated PC, so its drivers and caches read the new data
 * instead of what they held before.
 */

bool ftpui_is_open(void);
void ftpui_open(void);
void ftpui_handle_key(int keycode, bool is_down);

/* Redraw background and status; call at ~60 Hz while the page is open. */
void ftpui_animate(void);

#endif /* ORANGE386_FTPUI_H */
