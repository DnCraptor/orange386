#ifndef ORANGE386_HARDWARE_WATCHDOG_H
#define ORANGE386_HARDWARE_WATCHDOG_H

#include <stdint.h>
#include <pico.h>

/*
 * Linux host replacement for the Pico SDK <hardware/watchdog.h>.
 *
 * murm386 reboots the whole RP2350 through the watchdog (Disk Manager
 * "Save and Reboot", settings restart).  The host equivalent restarts the
 * emulator process: platform_reboot() (main.c) releases DRM, input grabs,
 * audio and guest RAM and re-executes the binary.  It does not return.
 */
void platform_reboot(void) __attribute__((noreturn));

static inline void watchdog_reboot(uint32_t pc, uint32_t sp, uint32_t delay_ms)
{
	(void)pc;
	(void)sp;
	(void)delay_ms;
	platform_reboot();
}

#endif /* ORANGE386_HARDWARE_WATCHDOG_H */
