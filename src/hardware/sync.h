#ifndef ORANGE386_HARDWARE_SYNC_H
#define ORANGE386_HARDWARE_SYNC_H

#include <stdint.h>

/*
 * Linux host replacement for the Pico SDK <hardware/sync.h>.
 *
 * On RP2350 these calls keep the core0 44.1 kHz timer interrupt (PIT/RTC
 * IRQ sources, main.c default_tsr0) from observing PIT state half-updated by
 * a guest port access.  The Linux port services those IRQ sources
 * synchronously on the emulation thread (pc.c pc_service_impl), so there is
 * no interrupt context to exclude.  Other host threads (audio) only read
 * device state, exactly like RP2350 core1, which these calls never masked.
 */
static inline uint32_t save_and_disable_interrupts(void)
{
	return 0;
}

static inline void restore_interrupts(uint32_t status)
{
	(void)status;
}

#endif /* ORANGE386_HARDWARE_SYNC_H */
