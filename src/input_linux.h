#ifndef ORANGE386_INPUT_LINUX_H
#define ORANGE386_INPUT_LINUX_H

#include <stdbool.h>
#include <stdint.h>

/*
 * evdev keyboards and mice (/dev/input/event*), the Linux counterpart of
 * murm386's PS/2 and USB HID wrappers.  Devices are grabbed exclusively so
 * key presses do not also reach the text console underneath the KMS output,
 * and are hot-plugged by periodic rescans.  Key codes are Linux input codes,
 * the same code space murm386's keyboard wrappers produce.
 */

void input_init(void);
void input_shutdown(void);

/* Rescan for new devices (rate-limited) and read pending events. */
void input_poll(void);

/* Next keyboard event: value 1 = press, 2 = autorepeat, 0 = release. */
bool input_get_key(int *is_down, int *keycode);

/* Accumulated mouse motion since the last call (dz: PS/2 sign, positive =
 * wheel towards the user) and the current button state (bit0 left,
 * bit1 right, bit2 middle).  Returns false when nothing changed. */
bool input_get_mouse(int16_t *dx, int16_t *dy, int8_t *dz, uint8_t *buttons);

#endif /* ORANGE386_INPUT_LINUX_H */
