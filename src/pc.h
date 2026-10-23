#ifndef PC_H
#define PC_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

#include "i386.h"
#include "i8259.h"
#include "i8254.h"
#include "i8042.h"
#include "misc.h"
#include "pcspk.h"
#include "pci.h"
#include "ini.h"

/*
 * Linux/ARM64 machine model shared with murm386 (src/pc.h).
 *
 * The PC structure keeps murm386's field names so device and BIOS sources can
 * be brought over unchanged.  Devices are added here as they are ported; the
 * RP2350 ARM32 ABI layout asserts do not apply to the LP64 host and are not
 * carried over.
 */

/// Platform HAL (platform_linux.c)
uint32_t get_uticks();
void *pcmalloc(long size);
int load_rom(void *phys_mem, const char *file, uword addr, int backward);

typedef void SimpleFBDrawFunc(void *opaque, int x, int y, int w, int h);

typedef struct PC {
	CPU *cpu;
	PicState2 *pic;
	PITState *pit;
	U8250 *serial;
	CMOS *cmos;
	int64_t boot_start_time;

	SimpleFBDrawFunc *redraw;
	void *redraw_data;
	void (*poll)(void *);

	KBDState *i8042;
	PS2KbdState *kbd;
	PS2MouseState *mouse;
	PCSpkState *pcspk;

	int pcspk_enabled;
	int mouse_enabled;

	I440FXState *i440fx;
	PCIBus *pcibus;

	const char *bios;
	const char *vga_bios;

	u8 port92;
	int shutdown_state;
	int reset_request;
	int paused;  // Emulation paused (e.g., for disk UI)

	int enable_serial;
	int fpu_enabled;       /* conf->fpu */
	int full_update;

	uint8_t lpt_data[2];   /* защёлки data-регистров LPT1(0x378)/LPT2(0x278) */
	uint8_t lpt_ctrl[2];   /* защёлки control-регистров 0x37A/0x27A  	*/
} PC;

typedef struct {
	const char *bios;
	const char *vga_bios;
	long mem_size;
	long vga_mem_size;
	const char *ata[4];
	int iscd[4];
	const char *fdd[2];
	int width;
	int height;
	int cpu_gen;
	int fpu;
	int enable_serial;
	int vga_force_8dm;
} PCConfig;

PC *pc_new(SimpleFBDrawFunc *redraw, void (*poll)(void *), void *redraw_data,
	   u8 *fb, PCConfig *conf);

void pc_step(PC *pc, size_t max_ops);
/* Service emulated devices without executing guest CPU instructions. */
void pc_service(PC *pc);

int parse_conf_ini(void* user, const char* section,
		   const char* name, const char* value);
void load_bios_and_reset(PC *pc);

#endif /* PC_H */
