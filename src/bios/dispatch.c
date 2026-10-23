#include <stdio.h>
#include <stdlib.h>
#include "i386.h"
#include "286/cpu.h"
#include "bios/bios.h"

/*
 * Native BIOS handler table and escape-stub dispatch.
 *
 * murm386 keeps these in src/286/cpu.c next to its 80286 core.  The Linux
 * port runs the 386 core only (I386_MODE), so only the table, its installer,
 * the diagnostics dump and the dispatcher called by the 0F FF <id> escape in
 * i386.c are brought over.
 */

handler_t handlers[256];

void cpu_err_msg(CPU* cpu, const char* msg) {
    char buf[80];
    uint32_t ip = ((uint32_t)CPU_CS << 4) + CPU_IP;
    uint32_t sp = ((uint32_t)CPU_SS << 4) + CPU_SP;
    uint16_t fw = cpu_getflags(cpu);
    print_line(msg, 1);

    snprintf(buf, sizeof(buf), "AX=%04X BX=%04X CX=%04X DX=%04X ",
             CPU_AX, CPU_BX, CPU_CX, CPU_DX); print_line(buf, 2);
    snprintf(buf, sizeof(buf), "SI=%04X DI=%04X BP=%04X SP=%04X ",
             CPU_SI, CPU_DI, CPU_BP, CPU_SP); print_line(buf, 3);
    snprintf(buf, sizeof(buf), "CS:IP=%04X:%04X  SS:SP=%04X:%04X ",
             CPU_CS, CPU_IP, CPU_SS, CPU_SP); print_line(buf, 4);
    snprintf(buf, sizeof(buf), "DS=%04X ES=%04X FS=%04X GS=%04X ",
             CPU_DS, CPU_ES,
             cpu->ext_accessors->get_seg16(cpu, SEG_FS),
             cpu->ext_accessors->get_seg16(cpu, SEG_GS));
    print_line(buf, 5);

    snprintf(buf, sizeof(buf), "FLAGS=%04X %c%c%c%c%c%c%c%c%c ",
             fw,
             (fw & 0x0800) ? 'O' : '-',
             (fw & 0x0400) ? 'D' : '-',
             (fw & 0x0200) ? 'I' : '-',
             (fw & 0x0100) ? 'T' : '-',
             (fw & 0x0080) ? 'S' : '-',
             (fw & 0x0040) ? 'Z' : '-',
             (fw & 0x0010) ? 'A' : '-',
             (fw & 0x0004) ? 'P' : '-',
             (fw & 0x0001) ? 'C' : '-');
    print_line(buf, 6);

    snprintf(buf, sizeof(buf), "OP=%02X %02X %02X %02X %02X %02X ",
             read86(ip + 0), read86(ip + 1), read86(ip + 2),
             read86(ip + 3), read86(ip + 4), read86(ip + 5));
    print_line(buf, 7);

    snprintf(buf, sizeof(buf), "STACK=%04X %04X %04X %04X ",
             readw86(sp + 0), readw86(sp + 2),
             readw86(sp + 4), readw86(sp + 6));
    print_line(buf, 8);

    snprintf(buf, sizeof(buf), "RET? IP=%04X CS=%04X FL=%04X ",
             readw86(sp + 0), readw86(sp + 2), readw86(sp + 4));
    print_line(buf, 9);
}

static bool no_handler(CPU* cpu) {
#ifdef NO_HANDLER_DETECTOR
    cpu_err_msg(cpu, "ERROR: no handler defined");
    abort();
#else
    (void)cpu;
#endif
    return true;
}

void cpu_install_bios_handlers(CPU* cpu) {
    (void)cpu;
    for (int i = 0; i < 256; ++i) {
        if (i == 0x21 || i == 0x29 || i == 0x2f) continue;
        handlers[i] = no_handler;
    }
    handlers[0x00] = bios_00h; // DIVIDE BY ZERO
    handlers[0x05] = bios_05h; // PRINT SCREEN / BOUND EXCEPTION
    handlers[0x08] = bios_08h; // IRQ0: Timer
    handlers[0x09] = bios_09h; // IRQ1: Keyboard
    handlers[0x10] = bios_10h; // VIDEO
    handlers[0x11] = bios_11h; // EQUIPMENT LIST
    handlers[0x12] = bios_12h; // Conventional RAM count
    handlers[0x13] = bios_13h; // DISK
    handlers[0x14] = bios_14h; // SERIAL
    handlers[0x15] = bios_15h; // TSR
    handlers[0x16] = bios_16h; // KEYBOARD
    handlers[0x17] = bios_17h; // PRINTERS
    handlers[0x18] = bios_18h; // BASIC
    handlers[0x19] = bios_19h; // BOOTSTRAP
    handlers[0x1A] = bios_1Ah; // CMOS TIME
    handlers[0x33] = bios_33h; // MS MOUSE
    handlers[0x74] = bios_74h; // IRQ12: PS/2 mouse
    handlers[0xFF] = bios_FFh; // W/A BIOS callback
}

/*
 * murm386's 80286 core keeps single-step trap and STI/MOV SS interrupt
 * shadow state that bios_intcall() and cpu_far_call() save around nested
 * guest execution.  The 386 core does not use them; the accessors stay so
 * the native BIOS sources are shared unchanged.
 */
static bool pending_ss_trap;

bool cpu_pending_trap(void)        { return pending_ss_trap; }
void cpu_pending_trap_set(bool v)  { pending_ss_trap = v; }

bool cpu_irq_shadow(const CPU* cpu) { return cpu->i286_irq_shadow; }
void cpu_irq_shadow_set(CPU* cpu, bool v) { cpu->i286_irq_shadow = v; }

#if !NATIVE_DOS
/*
 * Provided by the native DOS kernel in murm386 (fdos/task.c, fdos/kernel.c).
 * Until that kernel is built here, nothing can request a native process
 * termination, and INT 33h user handlers still need the far-call helper.
 */
bool terminate_requested(void)
{
    return false;
}

extern struct PC* pc;
void pc_step(struct PC* pc, size_t max_ops);

/* cpu_far_call_waiter() - fired when the CPU reaches back the synthetic
   return address pushed by cpu_far_call() (the callee did RETF).  A
   CALL/RETF pair never expects a flags-restoring IRET at the trap address,
   so this always returns false. */
static bool cpu_far_call_waiter(CPU* cpu, bios_callback_params_t* params) {
    if (!params->done) {
        ifl = 0; // no IRQ allowed this time
        params->done = true;
        cpu->native_done = true;
    }
    return false;
}

/* cpu_far_call(cpu, seg, off) - synchronously CALL FAR seg:off in guest
   code and wait for it to RETF back: push a return CS:IP that aliases the
   native callback sentinel at physical FFEFF (bios_FFh.c), set CS:IP to the
   target and run the CPU until its 0F FF FF escape reports the RETF. */
void cpu_far_call(CPU* cpu, uint16_t seg, uint16_t off)
{
    uint16_t save_cs = CPU_CS, save_ip = CPU_IP;
    bios_callback_params_t params = {
        .callback = cpu_far_call_waiter,
        .expected_cs = 0xFFEF,
        .expected_ip = 0x000F,
        .done = false,
    };
    /* См. bios_intcall(): native_done стекуется, внешнее состояние
       восстанавливается после вложенного цикла. */
    bool old_native_done = cpu->native_done;
    bool old_irq_shadow = cpu_irq_shadow(cpu);
    cpu->native_done = false;
    cpu_irq_shadow_set(cpu, false);
    set_bios_callback(cpu, &params, true);
    /* Emulate exactly what "CALL FAR seg:off" pushes: CS, then IP (so
       that RETF - which pops IP, then CS - lands back here). */
    CPU_SP -= 2;
    putmem8(CPU_SS, CPU_SP, params.expected_cs & 0xff);
    putmem8(CPU_SS, (CPU_SP + 1) & 0xffff, params.expected_cs >> 8);
    CPU_SP -= 2;
    putmem8(CPU_SS, CPU_SP, params.expected_ip & 0xff);
    putmem8(CPU_SS, (CPU_SP + 1) & 0xffff, params.expected_ip >> 8);

    SET_CS(seg);
    SET_IP(off);
    while (!params.done)
        pc_step(pc, 4096);
    cpu->native_done = old_native_done;
    drop_bios_callback(cpu, &params);
    cpu_irq_shadow_set(cpu, old_irq_shadow);
    SET_CS(save_cs);
    SET_IP(save_ip);
}
#endif /* !NATIVE_DOS */

/* murm386: rp2350_bios_handler() */
bool native_bios_dispatch(CPU* cpu, uint8_t intnum)
{
    /* The 386 core keeps arithmetic flags lazily (cc.mask) and folds them
     * into cpu->flags only inside get_flags().  The handlers set ZF/CF
     * through the cpu->flags.bits macros and then read them back with
     * cpu_getflags(), which would refold the stale lazy state over them.
     * Fold the pending state first.  An INT instruction already did that
     * when it pushed FLAGS, but code that emulates INT with PUSHF + far
     * CALL/RETF to the vector (Turbo Pascal Dos.Intr, debuggers, TSRs that
     * chain) reaches the stub with flags still lazy. */
    (void)cpu_getflags(cpu);
    return handlers[intnum](cpu);
}
