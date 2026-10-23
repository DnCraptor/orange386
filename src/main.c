#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>

#include "pc.h"
#include "input_linux.h"
#include "platform_linux.h"
#include "vga_hw_linux.h"
#include "video_drm.h"
#include "video_profile.h"

/* Linux host: Ctrl+C / SIGTERM stop emulation and restore the console CRTC. */
static volatile sig_atomic_t cpu_running = 1;

static void stop_cpu(int signum)
{
    (void)signum;
    cpu_running = 0;
}

static PCConfig config;
static PC *pc;
static bool reboot_requested;

/*
 * Full "hardware" reboot.  RP2350 reboots the whole MCU through the watchdog;
 * the Linux equivalent restarts the emulator process (main() re-executes
 * itself once DRM, input grabs and guest RAM are released), so every device
 * starts from its power-on state exactly as on the board.
 */
static void hard_reboot(void)
{
    reboot_requested = true;
    cpu_running = 0;
}

//=============================================================================
// Keyboard Polling
//=============================================================================

// Host-side modifier state. These hotkeys must remain available even when
// the guest CPU, PIC, 8042, or interrupt state is no longer usable.
static bool win_key_pressed = false;
static bool ctrl_key_pressed = false;
static bool alt_key_pressed = false;

/* Linux input keycodes (the same code space murm386's wrappers produce). */
enum {
    HOST_KEY_LEFTCTRL  = 29,
    HOST_KEY_LEFTALT   = 56,
    HOST_KEY_RIGHTCTRL = 97,
    HOST_KEY_RIGHTALT  = 100,
    HOST_KEY_END       = 107,
    HOST_KEY_DELETE    = 111,
    HOST_KEY_LEFTMETA  = 125
};

// Process a single keycode, handling host hotkeys
// Returns true if key should be passed to emulator, false if consumed
static bool process_keycode(int is_down, int keycode) {
    /*
     * Track Ctrl/Alt before anything else.  Ctrl+Alt+Del is a host-side
     * emergency reset and must work even while the guest is wedged.
     */
    if (keycode == HOST_KEY_LEFTCTRL || keycode == HOST_KEY_RIGHTCTRL)
        ctrl_key_pressed = is_down;
    if (keycode == HOST_KEY_LEFTALT || keycode == HOST_KEY_RIGHTALT)
        alt_key_pressed = is_down;

    if (is_down && keycode == HOST_KEY_DELETE &&
        ctrl_key_pressed && alt_key_pressed) {
        hard_reboot();
        return false;
    }

    /* Linux host only: Ctrl+Alt+End leaves the emulator.  Input devices are
     * grabbed exclusively, so the console's Ctrl+C cannot reach us. */
    if (is_down && keycode == HOST_KEY_END &&
        ctrl_key_pressed && alt_key_pressed) {
        cpu_running = 0;
        return false;
    }

    // Track Win key state
    if (keycode == HOST_KEY_LEFTMETA) {
        win_key_pressed = is_down;
    }

    return true;  // Pass to emulator
}

static void poll_keyboard(void) {
    int is_down, keycode;

    input_poll();

    while (input_get_key(&is_down, &keycode)) {
        if (process_keycode(is_down, keycode)) {
            if (pc && pc->kbd) {
                ps2_put_keycode(pc->kbd, is_down, keycode);
            }
        }
    }

    int16_t dx, dy;
    int8_t dz;
    uint8_t buttons;
    if (input_get_mouse(&dx, &dy, &dz, &buttons) && pc && pc->mouse &&
        pc->mouse_enabled && !pc->paused)
        ps2_mouse_event(pc->mouse, dx, dy, dz, buttons);
}

static void load_default_config(void)
{
    memset(&config, 0, sizeof(config));
    config.mem_size = 16l << 20;
    config.vga_mem_size = (long)video_profile_vram_size();
    config.cpu_gen = EMU_CPU_GEN;
    config.fpu = 1;
    config.width = 640;
    config.height = 400;
    /* NULL main BIOS means the native BIOS, which is not ported yet. */
    config.bios = NULL;
    config.vga_bios = "vgabios.bin";
}

/* VGA updates are handled by the scanout thread (vga_hw_linux.c). */
static void vga_redraw(void *opaque, int x, int y, int w, int h)
{
    (void)opaque; (void)x; (void)y; (void)w; (void)h;
}

static void platform_poll(void *opaque)
{
    (void)opaque;

    /*
     * platform_poll() is called from pc_step() after every CPU burst.
     * Input devices are read at most once per millisecond.
     */
    static uint32_t last_poll;
    uint32_t now = get_uticks();
    if ((uint32_t)(now - last_poll) >= 1000u) {
        last_poll = now;
        poll_keyboard();
    }
    // Guest reset (8042 pulse, port 92h) reboots the machine, as on RP2350
    if (pc && pc->reset_request) {
        pc->reset_request = 0;
        hard_reboot();
    }
}

static void run_pc(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = stop_cpu;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    puts("PC running; Ctrl+Alt+End (or Ctrl+C/SIGTERM) stops emulation, "
         "Ctrl+Alt+Del reboots.");
    fflush(stdout);
    while (cpu_running && !pc->shutdown_state) {
        for (int i = 0; i < 10 && cpu_running; i++)
            pc_step(pc, 4096);
    }
}

int main(int argc, char **argv)
{
    const char *device = argc > 1 ? argv[1] : "/dev/dri/card0";
    load_default_config();
    if (ini_parse(SD_DATA_DIR_SLASH "config.ini", parse_conf_ini, &config) < 0)
        fprintf(stderr, "Cannot read " SD_DATA_DIR_SLASH "config.ini, using defaults\n");
    if (!config.bios) {
        fprintf(stderr, "Native BIOS is not yet ported; set bios=bios.bin in "
                SD_DATA_DIR_SLASH "config.ini\n");
        return EXIT_FAILURE;
    }

    if (!platform_alloc_guest_ram((size_t)config.mem_size)) {
        perror("guest RAM allocation");
        return EXIT_FAILURE;
    }

    pc = pc_new(vga_redraw, platform_poll, NULL, NULL, &config);
    if (!pc || !pc->cpu) {
        fprintf(stderr, "Cannot initialize PC\n");
        platform_free_guest_ram();
        return EXIT_FAILURE;
    }
    load_bios_and_reset(pc);

    int result = EXIT_FAILURE;
    if (video_drm_open(device, VGA_HW_WIDTH, VGA_HW_HEIGHT)) {
        if (vga_hw_start(pc->vga)) {
            input_init();
            run_pc();
            input_shutdown();
            vga_hw_stop();
            result = EXIT_SUCCESS;
        }
        video_drm_close();
    }

    platform_free_guest_ram();
    if (reboot_requested) {
        fflush(NULL);
        execv("/proc/self/exe", argv);
        perror("reboot: execv");
        return EXIT_FAILURE;
    }
    return result;
}
