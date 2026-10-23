#ifndef ORANGE386_CORE0_STACK_H
#define ORANGE386_CORE0_STACK_H

/*
 * murm386 src/core0_stack.h: RP2350 core0 runs the native DOS kernel on a
 * few KiB of SRAM stack that can be relocated into the unused GFX buffer.
 * The Linux host runs it on the main thread stack, which needs no
 * relocation, so the post-init expansion has nothing to do.
 */
static inline void core0_expand_relocated_stack_services(void) { }

#endif /* ORANGE386_CORE0_STACK_H */
