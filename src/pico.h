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

/*
 * Data memory barrier.  murm386 uses CMSIS __dmb() to order lock-free
 * producer/consumer queues between core0 and core1 (AdLib); on the host the
 * producer and consumer are threads, so a full fence gives the same order.
 */
/* Pico SDK multiply intrinsic (RP2350: guaranteed single-cycle MUL). */
#ifndef __fast_mul
#define __fast_mul(a, b) ((a) * (b))
#endif

#ifndef __dmb
#define __dmb() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#endif

#endif /* ORANGE386_PICO_H */
