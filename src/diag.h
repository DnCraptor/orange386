#ifndef ORANGE386_DIAG_H
#define ORANGE386_DIAG_H

/*
 * murm386 src/diag.h: core1 watchdog/heartbeat diagnostics of the RP2350
 * build.  Only the native-code bracketing hooks are referenced by shared
 * sources; on the host they have nothing to report.
 */
static inline void diag_native_code_enter(void) { }
static inline void diag_native_code_leave(void) { }

#endif /* ORANGE386_DIAG_H */
