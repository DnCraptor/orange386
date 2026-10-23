#include "pc.h"
#include "mem.h"
#include "vga_text.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

/*
 * Linux/ARM64 machine model, ported from murm386 src/pc.c.
 *
 * Port dispatch and device construction follow murm386 one-to-one for every
 * device already brought over.  Devices that are not ported yet answer like
 * an empty ISA slot (reads 0xFF, writes ignored), exactly as murm386 does for
 * disabled devices.  VGA ports are temporarily routed to the bootstrap text
 * renderer (vga_text.c) until vga.c is ported.
 */

unsigned long phys_mem_size = 16ul << 20;
void* g_pc;

static __always_inline u8 _pc_io_read(void *o, int addr)
{
	PC *pc = o;
	u8 val;

	switch(addr) {
	case 0x20: case 0x21: case 0xa0: case 0xa1:
		val = i8259_ioport_read(pc->pic, addr);
		return val;
	case 0x3f8: case 0x3f9: case 0x3fa: case 0x3fb:
	case 0x3fc: case 0x3fd: case 0x3fe: case 0x3ff:
		val = 0xff;
		if (pc->enable_serial)
			val = u8250_reg_read(pc->serial, addr - 0x3f8);
		return val;
	case 0x2f8: case 0x2f9: case 0x2fa: case 0x2fb:
	case 0x2fc: case 0x2fd: case 0x2fe: case 0x2ff:
	case 0x2e8: case 0x2e9: case 0x2ea: case 0x2eb:
	case 0x2ec: case 0x2ed: case 0x2ee: case 0x2ef:
	case 0x3e8: case 0x3e9: case 0x3ea: case 0x3eb:
	case 0x3ec: case 0x3ed: case 0x3ee: case 0x3ef:
		return 0xff;
	case 0x42:
		/* read delay for PIT channel 2 */
		/* certain guest code needs it to drive pc speaker properly */
		usleep(0);
		/* fall through */
	case 0x40: case 0x41: case 0x43:
		val = i8254_ioport_read(pc->pit, addr);
		return val;
	case 0x70: case 0x71:
		val = cmos_ioport_read(pc->cmos, addr);
		return val;
	case 0x3c0: case 0x3c1: case 0x3c2: case 0x3c3:
	case 0x3c4: case 0x3c5: case 0x3c6: case 0x3c7:
	case 0x3c8: case 0x3c9: case 0x3ca: case 0x3cb:
	case 0x3cc: case 0x3cd: case 0x3ce: case 0x3cf:
	case 0x3d0: case 0x3d1: case 0x3d2: case 0x3d3:
	case 0x3d4: case 0x3d5: case 0x3d6: case 0x3d7:
	case 0x3d8: case 0x3d9: case 0x3da: case 0x3db:
	case 0x3dc: case 0x3dd: case 0x3de: case 0x3df:
		return vga_text_io_read((uint16_t)addr);
	case 0x92:
		return pc->port92 | 0x02;
	case 0x60:
		val = kbd_read_data(pc->i8042, addr);
		return val;
	case 0x64:
		val = kbd_read_status(pc->i8042, addr);
		return val;
	case 0x61:
		val = pcspk_ioport_read(pc->pcspk);
		return val;
	case 0xcfc: case 0xcfd: case 0xcfe: case 0xcff:
		val = i440fx_read_data(pc->i440fx, addr - 0xcfc, 0);
		return val;
	case 0x200: case 0x201: case 0x202: case 0x203:
	case 0x204: case 0x205: case 0x206: case 0x207:
		/* Empty game port adapter: axes timed out, no buttons. */
		return 0xf0;

	/* --- LPT1 (0x378) / LPT2 (0x278): printer latches -------------------
	 * status: bit7 nBusy=1, bit4 Select=1, bit3 nError=1 (printer ready).
	 */
	case 0x378: return pc->lpt_data[0];
	case 0x379: return 0xD8;
	case 0x37A: return pc->lpt_ctrl[0];
	case 0x278: return pc->lpt_data[1];
	case 0x279: return 0xD8;                 /* idle: nBusy|nAck|Select|nError */
	case 0x27A: return pc->lpt_ctrl[1];
	default:
		return 0xff;
	}
}

static u8 pc_io_read(void *o, int addr) {
	return _pc_io_read(o, addr);
}

static u16 pc_io_read16(void *o, int addr)
{
	PC *pc = o;
	u16 val;

	switch(addr) {
	case 0xcf8:
		val = i440fx_read_addr(pc->i440fx, 0, 1);
		return val;
	case 0xcfc: case 0xcfe:
		val = i440fx_read_data(pc->i440fx, addr - 0xcfc, 1);
		return val;
	case 0x200: case 0x201: case 0x202: case 0x203:
	case 0x204: case 0x205: case 0x206: case 0x207:
		return 0xfff0u;
	default:
		return 0;
	}
}

static u32 pc_io_read32(void *o, int addr)
{
	PC *pc = o;
	u32 val;
	switch(addr) {
	case 0x3cc:
		return (get_uticks() - pc->boot_start_time) / 1000;
	case 0xcf8:
		val = i440fx_read_addr(pc->i440fx, 0, 2);
		return val;
	case 0xcfc:
		val = i440fx_read_data(pc->i440fx, 0, 2);
		return val;
	default:
		return 0;
	}
}

static int pc_io_read_string(void *o, int addr, uint32_t buf, int size, int count)
{
	(void)o; (void)addr; (void)buf; (void)size; (void)count;
	return 0;
}

static void pc_io_write(void *o, int addr, u8 val)
{
	PC *pc = o;
	switch(addr) {
	case 0x80: case 0xed:
		/* used by linux, for io delay */
		return;
	case 0x20: case 0x21: case 0xa0: case 0xa1:
		i8259_ioport_write(pc->pic, addr, val);
		return;
	case 0x3f8: case 0x3f9: case 0x3fa: case 0x3fb:
	case 0x3fc: case 0x3fd: case 0x3fe: case 0x3ff:
		if (pc->enable_serial)
			u8250_reg_write(pc->serial, addr - 0x3f8, val);
		return;
	case 0x40: case 0x41: case 0x42: case 0x43:
		i8254_ioport_write(pc->pit, addr, val);
		return;
	case 0x70: case 0x71:
		cmos_ioport_write(pc->cmos, addr, val);
		return;
	case 0x3c0: case 0x3c1: case 0x3c2: case 0x3c3:
	case 0x3c4: case 0x3c5: case 0x3c6: case 0x3c7:
	case 0x3c8: case 0x3c9: case 0x3ca: case 0x3cb:
	case 0x3cc: case 0x3cd: case 0x3ce: case 0x3cf:
	case 0x3d0: case 0x3d1: case 0x3d2: case 0x3d3:
	case 0x3d4: case 0x3d5: case 0x3d6: case 0x3d7:
	case 0x3d8: case 0x3d9: case 0x3da: case 0x3db:
	case 0x3dc: case 0x3dd: case 0x3de: case 0x3df:
		vga_text_io_write((uint16_t)addr, val);
		return;
	case 0x402:
		return;
	case 0x92:
		/* Fast A20 gate is hard-wired on; preserve all other port bits. */
		pc->port92 = val | 0x02;
		cpu_set_a20(pc->cpu, 1);
		return;
	case 0x60:
		kbd_write_data(pc->i8042, addr, val);
		return;
	case 0x64:
		kbd_write_command(pc->i8042, addr, val);
		return;
	case 0x61:
		pcspk_ioport_write(pc->pcspk, val);
		return;
	case 0x8900:
		switch (val) {
		case 'S': if (pc->shutdown_state == 0) pc->shutdown_state = 1; break;
		case 'h': if (pc->shutdown_state == 1) pc->shutdown_state = 2; break;
		case 'u': if (pc->shutdown_state == 2) pc->shutdown_state = 3; break;
		case 't': if (pc->shutdown_state == 3) pc->shutdown_state = 4; break;
		case 'd': if (pc->shutdown_state == 4) pc->shutdown_state = 5; break;
		case 'o': if (pc->shutdown_state == 5) pc->shutdown_state = 6; break;
		case 'w': if (pc->shutdown_state == 6) pc->shutdown_state = 7; break;
		case 'n': if (pc->shutdown_state == 7) pc->shutdown_state = 8; break;
		default : pc->shutdown_state = 0; break;
		}
		return;
	case 0xcfc: case 0xcfd: case 0xcfe: case 0xcff:
		i440fx_write_data(pc->i440fx, addr - 0xcfc, val, 0);
		return;
	case 0x278:
		pc->lpt_data[1] = val;              /* latch! */
		return;
	case 0x27A:
		pc->lpt_ctrl[1] = val & 0x1F;
		return;
	case 0x378:
		pc->lpt_data[0] = val;              /* latch! */
		return;
	case 0x37A:
		pc->lpt_ctrl[0] = val & 0x1F;
		return;
	/* LPT status/control ports are read-only - writes ignored */
	case 0x379: case 0x279:
	default:
		return;
	}
}

static void pc_io_write16(void *o, int addr, u16 val)
{
	PC *pc = o;
	switch(addr) {
	case 0x3c0: case 0x3c1: case 0x3c2: case 0x3c3:
	case 0x3c4: case 0x3c5: case 0x3c6: case 0x3c7:
	case 0x3c8: case 0x3c9: case 0x3ca: case 0x3cb:
	case 0x3cc: case 0x3cd: case 0x3ce: case 0x3cf:
	case 0x3d0: case 0x3d1: case 0x3d2: case 0x3d3:
	case 0x3d4: case 0x3d5: case 0x3d6: case 0x3d7:
	case 0x3d8: case 0x3d9: case 0x3da: case 0x3db:
	case 0x3dc: case 0x3dd: case 0x3de:
		pc_io_write(o, addr, val & 0xff);
		pc_io_write(o, addr + 1, (val >> 8) & 0xff);
		return;
	case 0xcfc: case 0xcfe:
		i440fx_write_data(pc->i440fx, addr - 0xcfc, val, 1);
		return;
	default:
		return;
	}
}

static void pc_io_write32(void *o, int addr, u32 val)
{
	PC *pc = o;
	switch(addr) {
	case 0xcf8:
		i440fx_write_addr(pc->i440fx, 0, val, 2);
		return;
	case 0xcfc:
		i440fx_write_data(pc->i440fx, 0, val, 2);
		return;
	default:
		return;
	}
}

static int pc_io_write_string(void *o, int addr, uint32_t buf, int size, int count)
{
	(void)o; (void)addr; (void)buf; (void)size; (void)count;
	return 0;
}

static uint32_t pc_last_device_service;

static void pc_service_impl(PC *pc)
{
	pc_last_device_service = get_uticks();

	/*
	 * RP2350 drives the PIT and RTC interrupt sources from a 44.1 kHz core0
	 * timer interrupt (main.c default_tsr0).  The Linux port has no
	 * asynchronous interrupt context: the same sources are serviced here,
	 * synchronously after every CPU burst.
	 */
	i8254_update_irq(pc->pit);
	cmos_update_irq(pc->cmos);

	if (pc->enable_serial)
		u8250_update(pc->serial);
	kbd_step(pc->i8042);

	if (pc->poll) pc->poll(pc->redraw_data);
}

void pc_service(PC *pc)
{
	pc_service_impl(pc);
}

void pc_step(PC *pc, size_t max_ops)
{
	bool was_native_done = pc->cpu->native_done;
	if (!pc->paused) {
		if (max_ops > 4096) max_ops = 4096;
		cpu_step(pc->cpu, max_ops);
		if (pc->cpu->native_done && !was_native_done &&
		    (uint32_t)(get_uticks() - pc_last_device_service) < 1000u)
			return;
	}

	pc_service_impl(pc);
}

static void raise_irq(void *o, PicState2 *s)
{
	cpu_raise_irq(o);
}

static int read_irq(void *o)
{
	PicState2 *s = o;
	return i8259_read_irq(s);
}

static void set_irq(void *o, int irq, int level)
{
	PicState2 *s = o;
	return i8259_set_irq(s, irq, level);
}

static void pc_reset_request(void *p)
{
	PC *pc = p;
	pc->reset_request = 1;
}

PC *pc_new(SimpleFBDrawFunc *redraw, void (*poll)(void *), void *redraw_data,
	   u8 *fb, PCConfig *conf)
{
	(void)fb;
	PC *pc = calloc(1, sizeof(PC));
	g_pc = pc;
	CPU_CB *cb = NULL;
	memset(PC_RAM, 0, (size_t)conf->mem_size);
	phys_mem_size = conf->mem_size;
	pc->cpu = cpu_new(conf->cpu_gen, &cb);
	pc->fpu_enabled = conf->fpu ? 1 : 0;
	if (conf->fpu)
		enable_fpu(pc->cpu);
	pc->bios = conf->bios;
	pc->cpu->bios = pc->bios;
	pc->vga_bios = conf->vga_bios;
	pc->cpu->native_done = false;
	pc->enable_serial = conf->enable_serial;
	if (pc->enable_serial)
		CaptureKeyboardInput();
	pc->full_update = 0;

	pc->pic = i8259_init(raise_irq, pc->cpu);
	cb->pic = pc->pic;
	cb->pic_read_irq = read_irq;

	pc->pit = i8254_init(0, pc->pic, set_irq);
	pc->serial = u8250_init(4, pc->pic, set_irq);
	pc->cmos = cmos_init(conf->mem_size, 8, pc->pic, set_irq);

	/* CMOS 0x10 — типы FDD: дисководы пока не перенесены */
	cmos_set(pc->cmos, 0x10, 0x00);

	/* CMOS 0x14 — equipment byte:
	 *   bit 1     : math coprocessor
	 *   bit 2     : keyboard present
	 *   bit 3     : display enabled
	 *   bits 5-4  : 00 = EGA/VGA
	 */
	uint8_t eq = 0x04 | 0x08;
	if (conf->fpu) eq |= 0x02;
	cmos_set(pc->cmos, 0x14, eq);

	cmos_update_checksum(pc->cmos);

	int piix3_devfn;
	pc->i440fx = i440fx_init(&pc->pcibus, &piix3_devfn);

	cb->io = pc;
	cb->io_read8 = pc_io_read;
	cb->io_write8 = pc_io_write;
	cb->io_read16 = pc_io_read16;
	cb->io_write16 = pc_io_write16;
	cb->io_read32 = pc_io_read32;
	cb->io_write32 = pc_io_write32;
	cb->io_read_string = pc_io_read_string;
	cb->io_write_string = pc_io_write_string;

	pc->boot_start_time = 0;

	cb->iomem = pc;

	pc->redraw = redraw;
	pc->redraw_data = redraw_data;
	pc->poll = poll;

	pc->i8042 = i8042_init(&(pc->kbd), &(pc->mouse),
			       1, 12, pc->pic, set_irq,
			       pc, pc_reset_request);
	i8042_set_cpu(pc->cpu);
	pc->pcspk = pcspk_init(pc->pit);

	pc->pcspk_enabled = 1;
	pc->mouse_enabled = 1;

	/* LPT: защёлки data/control. control=0x04 (nInit=1). */
	pc->lpt_data[0] = pc->lpt_data[1] = 0xFF;
	pc->lpt_ctrl[0] = pc->lpt_ctrl[1] = 0x04;

	pc->port92 = 0x2;
	pc->shutdown_state = 0;
	pc->reset_request = 0;
	return pc;
}

void load_bios_and_reset(PC *pc)
{
	int bios_size = 0;
	if (pc->bios && pc->bios[0]) {
		bios_size = load_rom(PC_RAM, pc->bios, 0x100000, 1);

		// Only load VGA BIOS if main BIOS doesn't overlap with 0xC0000
		// 256KB BIOS starts at 0xC0000, so VGA BIOS would overwrite it
		int bios_start = 0x100000 - bios_size;
		if (pc->vga_bios && pc->vga_bios[0] && bios_start >= 0xC8000) {
			load_rom(PC_RAM, pc->vga_bios, 0xc0000, 0);
		} else if (pc->vga_bios && pc->vga_bios[0]) {
			printf("Skipping VGA BIOS - main BIOS overlaps at 0x%x\n", bios_start);
		}
	}
	cpu_reset(pc->cpu);
}

int parse_conf_ini(void* user, const char* section,
		   const char* name, const char* value)
{
	PCConfig *conf = user;
#define SEC(a) (strcmp(section, a) == 0)
#define NAME(a) (strcmp(name, a) == 0)
	// Accept [pc], [386] and the build's own [SD_DATA_DIR] section.
	if (SEC("pc") || SEC("386") || SEC(SD_DATA_DIR)) {
		if (NAME("bios")) {
			if (value[0] == '\0' || strcasecmp(value, "native") == 0)
				conf->bios = NULL;
			else
				conf->bios = strdup(value);
		} else if (NAME("vga_bios")) {
			conf->vga_bios = strdup(value);
		} else if (NAME("mem_size") || NAME("mem")) {
			/* Guest RAM in MiB (Linux host: no PSRAM to detect). */
			long mb = atol(value);
			if (mb >= 1 && mb <= 3072)
				conf->mem_size = mb << 20;
		} else if (NAME("cpu")) {
			conf->cpu_gen = atoi(value);
		} else if (NAME("hda")) {
			conf->ata[0] = strdup(value);
			conf->iscd[0] = 0;
		} else if (NAME("hdb")) {
			conf->ata[1] = strdup(value);
			conf->iscd[1] = 0;
		} else if (NAME("hdc")) {
			conf->ata[2] = strdup(value);
			conf->iscd[2] = 0;
		} else if (NAME("hdd")) {
			conf->ata[3] = strdup(value);
			conf->iscd[3] = 0;
		} else if (NAME("cda")) {
			conf->ata[0] = strdup(value);
			conf->iscd[0] = 1;
		} else if (NAME("cdb")) {
			conf->ata[1] = strdup(value);
			conf->iscd[1] = 1;
		} else if (NAME("cdc")) {
			conf->ata[2] = strdup(value);
			conf->iscd[2] = 1;
		} else if (NAME("cdd")) {
			conf->ata[3] = strdup(value);
			conf->iscd[3] = 1;
		} else if (NAME("fda")) {
			conf->fdd[0] = strdup(value);
		} else if (NAME("fdb")) {
			conf->fdd[1] = strdup(value);
		} else if (NAME("enable_serial")) {
			conf->enable_serial = atoi(value);
		} else if (NAME("vga_force_8dm")) {
			conf->vga_force_8dm = atoi(value);
		}
	} else if (SEC("display")) {
		if (NAME("width")) {
			conf->width = atoi(value);
		} else if (NAME("height")) {
			conf->height = atoi(value);
		}
	} else if (SEC("cpu")) {
		if (NAME("gen")) {
			conf->cpu_gen = atoi(value);
		} else if (NAME("fpu")) {
			conf->fpu = atoi(value);
		}
	}
#undef SEC
#undef NAME
	return 1;
}
