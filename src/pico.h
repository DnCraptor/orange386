#ifndef ORANGE386_PICO_H
#define ORANGE386_PICO_H

/*
 * Linux host replacement for the Pico SDK <pico.h>.
 *
 * Sources shared with murm386 annotate hot functions and data with RP2350
 * section placement macros (SRAM, scratch banks, flash exclusion).  On the
 * ARM64 host every function and object lives in ordinary process memory, so
 * the annotations reduce to the bare declaration.
 */

#define __not_in_flash(group)
#define __not_in_flash_func(func_name) func_name
#define __no_inline_not_in_flash_func(func_name) __attribute__((noinline)) func_name
#define __time_critical_func(func_name) func_name
#define __scratch_x(group)
#define __scratch_y(group)
#define __isr

#endif /* ORANGE386_PICO_H */
