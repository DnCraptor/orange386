#ifndef ORANGE386_VGA_HW_LINUX_H
#define ORANGE386_VGA_HW_LINUX_H

#include <stdbool.h>
#include <stdint.h>
#include "vga.h"

/*
 * Linux scanout for the emulated VGA: the counterpart of murm386's
 * drivers/vga/vga_hw.c + drivers/hdmi/hdmi.c (core1 DMA ISR renderers).
 *
 * The guest VGA runs on a virtual 640x480@59.94 timeline (800x525 pixel
 * clocks at 25.175 MHz) derived from CLOCK_MONOTONIC.  Status register 1
 * (port 3DAh) is computed from that timeline on demand.  A render thread
 * latches the per-frame CRTC state late in the virtual vblank, exactly where
 * the RP2350 ISR does (line 521), renders the whole frame into a 640x480
 * XRGB8888 raster and hands it to the DRM/KMS output.
 */

#define VGA_HW_WIDTH  640
#define VGA_HW_HEIGHT 480

/* Refresh st01 (V_RETRACE / DISP_ENABLE) from the virtual scanline clock. */
void vga_hw_update_st01(VGAState *s);

/* Start/stop the scanout thread for an initialised VGAState. */
bool vga_hw_start(VGAState *s);
void vga_hw_stop(void);

#endif /* ORANGE386_VGA_HW_LINUX_H */
