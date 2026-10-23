#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "pc.h"
#include "audio.h"
#include "board_config.h"
#include "config_save.h"
#include "diskui.h"
#include "settingsui.h"
#include "ff.h"
#include "gameport.h"
#include "input_linux.h"
#include "mem.h"
#include <hardware/timer.h>
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
PC *pc;
static bool reboot_requested;
static char **saved_argv;

/* Set across the execv() of an emulator reboot: the next POST is warm. */
#define WARM_BOOT_ENV "ORANGE386_WARM_BOOT"

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

/* Release everything the process holds outside itself: input grabs, the
 * scanout thread and CRTC, the PCM device and guest RAM.  Idempotent. */
static void host_shutdown(void)
{
    input_shutdown();
    vga_hw_stop();
    video_drm_close();
    audio_shutdown();
    platform_free_guest_ram();
}

/* watchdog_reboot() on the host (hardware/watchdog.h): immediate reboot from
 * any call depth, e.g. Disk Manager "Save and Reboot". */
void platform_reboot(void)
{
    host_shutdown();
    fflush(NULL);
    setenv(WARM_BOOT_ENV, "1", 1);
    execv("/proc/self/exe", saved_argv);
    perror("reboot: execv");
    _exit(EXIT_FAILURE);
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
    HOST_KEY_DELETE    = 111
};

/*
 * NumPad mouse: with the setting on, the numeric keypad drives the emulated
 * PS/2 mouse for machines without a real one.  8/2/4/6 (and 5 as down) move
 * the cursor, 7/9/1/3 move diagonally, keypad Enter is the left button and
 * keypad 0 the right one.  It works only while the guest's NumLock is off
 * (BDA 40:17h bit 5, which both BIOSes keep), so NumLock is a quick way to
 * get the digits back without opening the settings.
 *
 * Whether a key belongs to the mouse is decided on its press and remembered,
 * so its release always goes the same way: toggling NumLock or the setting
 * while a key is held can never leave a key stuck in the guest.
 */
enum {
    NUMPAD_KEY_7 = 71, NUMPAD_KEY_8 = 72, NUMPAD_KEY_9 = 73,
    NUMPAD_KEY_4 = 75, NUMPAD_KEY_5 = 76, NUMPAD_KEY_6 = 77,
    NUMPAD_KEY_1 = 79, NUMPAD_KEY_2 = 80, NUMPAD_KEY_3 = 81,
    NUMPAD_KEY_0 = 82, NUMPAD_KEY_ENTER = 96
};
enum {
    NPM_LEFT = 1u << 0, NPM_RIGHT = 1u << 1, NPM_UP = 1u << 2,
    NPM_DOWN = 1u << 3, NPM_BTN_L = 1u << 4, NPM_BTN_R = 1u << 5
};

typedef struct {
    uint8_t held[11];    /* direction/button bits of each held keypad key */
    uint16_t owned;      /* keypad keys whose press went to the mouse */
} NumpadMouseKeys;

static NumpadMouseKeys numpad_mouse_keys;

static int numpad_mouse_index(int keycode, uint8_t *bits)
{
    static const struct { uint8_t keycode, bits; } map[11] = {
        { NUMPAD_KEY_7, NPM_LEFT | NPM_UP },   { NUMPAD_KEY_8, NPM_UP },
        { NUMPAD_KEY_9, NPM_RIGHT | NPM_UP },  { NUMPAD_KEY_4, NPM_LEFT },
        { NUMPAD_KEY_5, NPM_DOWN },            { NUMPAD_KEY_6, NPM_RIGHT },
        { NUMPAD_KEY_1, NPM_LEFT | NPM_DOWN }, { NUMPAD_KEY_2, NPM_DOWN },
        { NUMPAD_KEY_3, NPM_RIGHT | NPM_DOWN },
        { NUMPAD_KEY_0, NPM_BTN_R },           { NUMPAD_KEY_ENTER, NPM_BTN_L }
    };
    for (int i = 0; i < 11; ++i) {
        if (map[i].keycode == keycode) {
            *bits = map[i].bits;
            return i;
        }
    }
    return -1;
}

/* true: the key was taken by the NumPad mouse and must not reach the guest */
static bool numpad_mouse_key(int is_down, int keycode)
{
    uint8_t bits;
    const int i = numpad_mouse_index(keycode, &bits);
    if (i < 0)
        return false;

    const uint16_t mask = (uint16_t)(1u << i);
    if (!is_down) {
        if (!(numpad_mouse_keys.owned & mask))
            return false;
        numpad_mouse_keys.owned &= (uint16_t)~mask;
        numpad_mouse_keys.held[i] = 0;
        return true;
    }

    if (!(numpad_mouse_keys.owned & mask)) {   /* a new press, not a repeat */
        if (!config_get_numpad_mouse() || !pc || !pc->mouse ||
            (pload8(0x417) & 0x20u))
            return false;
        numpad_mouse_keys.owned |= mask;
    }
    numpad_mouse_keys.held[i] = bits;
    return true;
}

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
    if (keycode == KEY_LEFTMETA) {
        win_key_pressed = is_down;
    }

    // Win+F12: enter Disk Manager, or switch to it from Settings.
    if (is_down && keycode == KEY_F12 && win_key_pressed) {
        if (settingsui_is_open()) {
            settingsui_close();
            diskui_open();
        } else if (!diskui_is_open()) {
            diskui_open();
            if (pc) {
                pc->paused = 1;
                audio_set_enabled(false);
            }
        }
        return false;  // Don't pass to emulator
    }

    // Win+F11: enter Settings, or switch to it from Disk Manager.
    if (is_down && keycode == KEY_F11 && win_key_pressed) {
        if (diskui_is_open()) {
            diskui_close();
            settingsui_open();
        } else if (!settingsui_is_open()) {
            settingsui_open();
            if (pc) {
                pc->paused = 1;
                audio_set_enabled(false);
            }
        }
        return false;  // Don't pass to emulator
    }

    // When disk UI is open, route all keys to it
    if (diskui_is_open()) {
        diskui_handle_key(keycode, is_down);

        // Leaving the OSD session resumes the guest.
        // F11/F12 page switches do not exit OSD.
        if (!diskui_is_open() && pc && pc->paused) {
            pc->paused = 0;
            audio_set_enabled(true);
        }
        return false;  // Don't pass to emulator
    }

    // When settings UI is open, route all keys to it
    if (settingsui_is_open()) {
        settingsui_handle_key(keycode, is_down);

        // Same OSD-session exit rule regardless of the page used last.
        if (!settingsui_is_open() && pc && pc->paused) {
            pc->paused = 0;
            audio_set_enabled(true);
        }
        return false;  // Don't pass to emulator
    }

    if (numpad_mouse_key(is_down, keycode))
        return false;  // Keypad key drives the emulated mouse

    return true;  // Pass to emulator
}

typedef struct {
    int32_t rem_x, rem_y;
    int setting;
} MouseScaleState;

typedef struct {
    int16_t x, y;
    uint8_t buttons;
    uint32_t last_event_us;
    bool was_enabled;
} MouseJoystickState;

static MouseJoystickState mouse_joystick_state;

static int16_t mouse_joystick_axis_from_delta(int16_t delta)
{
    /* Mouse motion is relative. Treat report velocity as stick deflection.
     * A one-mickey sensor jitter is a dead zone; 16 mickeys/report reaches
     * full travel. */
    if (delta >= -1 && delta <= 1)
        return 0;
    int32_t value = (int32_t)delta * 2048;
    if (value < -32768) value = -32768;
    if (value >  32767) value =  32767;
    return (int16_t)value;
}

static uint8_t mouse_joystick_buttons(uint8_t mouse_buttons)
{
    /* Keep the normal mouse buttons untouched, but swap them on the game
     * port: right mouse button is joystick button 1, left is button 2.
     * This prevents an ordinary left-click confirmation from also looking
     * like the primary joystick button. */
    return (uint8_t)(((mouse_buttons & 0x01u) << 1) |
                     ((mouse_buttons & 0x02u) >> 1));
}

static void mouse_joystick_event(int16_t dx, int16_t dy, uint8_t buttons)
{
    /* This function is called for a real mouse report.  Zero deltas are
     * meaningful: they are the report that returns the virtual stick to
     * centre after the physical mouse stops. */
    mouse_joystick_state.x = mouse_joystick_axis_from_delta(dx);
    mouse_joystick_state.y = mouse_joystick_axis_from_delta(dy);
    mouse_joystick_state.buttons = mouse_joystick_buttons(buttons);
    mouse_joystick_state.last_event_us = time_us_32();
}

static void mouse_scale_delta(MouseScaleState *st, int16_t *dx, int16_t *dy)
{
    static const uint8_t num[9] = { 1, 1, 1, 2, 1, 3, 2, 3, 4 };
    static const uint8_t den[9] = { 4, 3, 2, 3, 1, 2, 1, 1, 1 };
    int setting = config_get_mouse_sensitivity();

    if (setting < 0 || setting > 8)
        setting = 4;
    if (st->setting != setting) {
        st->setting = setting;
        st->rem_x = st->rem_y = 0;
    }

    if (setting == 4)
        return;

    int32_t ax = st->rem_x + (int32_t)*dx * num[setting];
    int32_t ay = st->rem_y + (int32_t)*dy * num[setting];
    int32_t ox = ax / den[setting];
    int32_t oy = ay / den[setting];
    st->rem_x = ax - ox * den[setting];
    st->rem_y = ay - oy * den[setting];
    *dx = (int16_t)ox;
    *dy = (int16_t)oy;
}

/*
 * NumPad mouse motion.  Like a NES pad, a key is level-sensing, so motion is
 * paced by time, not by poll rate: one step every 20 ms.  A short press moves
 * one mickey (fine positioning); a held key speeds up after 100 ms by a
 * quarter mickey per step up to 8 mickeys per step, so the cursor crosses
 * a 640-pixel screen in about a second.  The common Mouse speed scaler is
 * applied afterwards, as for every other mouse source.
 */
#define NUMPAD_MOUSE_PERIOD_US 20000u
#define NUMPAD_MOUSE_HOLD      5
#define NUMPAD_MOUSE_STEP_MIN  4    /* quarter mickeys */
#define NUMPAD_MOUSE_STEP_MAX  32

static void numpad_mouse_tick(void)
{
    static uint64_t next_move_us = 0;
    static int step_q = NUMPAD_MOUSE_STEP_MIN;
    static int held_steps = 0;
    static uint8_t prev_buttons = 0;
    static MouseScaleState mouse_scale = { .setting = -1 };

    if (!pc || !pc->mouse)
        return;
    if (pc->paused) {
        /* Releases made while an OSD menu is open go to the menu, so forget
         * the held keys; a held button is released on the next tick. */
        memset(&numpad_mouse_keys, 0, sizeof(numpad_mouse_keys));
        return;
    }

    uint8_t bits = 0;
    for (int i = 0; i < 11; ++i)
        bits |= numpad_mouse_keys.held[i];
    if (!bits && !prev_buttons)
        return;

    int16_t dx = 0, dy = 0;
    if (bits & (NPM_LEFT | NPM_RIGHT | NPM_UP | NPM_DOWN)) {
        const uint64_t now = time_us_64();
        if (!next_move_us || now >= next_move_us) {
            if (++held_steps > NUMPAD_MOUSE_HOLD && step_q < NUMPAD_MOUSE_STEP_MAX)
                ++step_q;
            const int16_t n = (int16_t)(step_q / 4);
            if (bits & NPM_LEFT)  dx -= n;
            if (bits & NPM_RIGHT) dx += n;
            if (bits & NPM_UP)    dy -= n;
            if (bits & NPM_DOWN)  dy += n;
            next_move_us = now + NUMPAD_MOUSE_PERIOD_US;
        }
    } else {
        next_move_us = 0;
        held_steps = 0;
        step_q = NUMPAD_MOUSE_STEP_MIN;
    }

    uint8_t buttons = 0;
    if (bits & NPM_BTN_L) buttons |= 0x01;
    if (bits & NPM_BTN_R) buttons |= 0x02;

    if ((dx || dy || buttons != prev_buttons) &&
        config_get_mouse_joystick() != MOUSE_JOYSTICK_ONLY) {
        mouse_scale_delta(&mouse_scale, &dx, &dy);
        ps2_mouse_event(pc->mouse, dx, dy, 0, buttons);
    }
    prev_buttons = buttons;
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

    // Poll evdev mice when used either as a PC mouse or as a game-port stick.
    if (pc && (config_get_mouse() || config_get_mouse_joystick()) && !pc->paused) {
        int16_t dx, dy;
        int8_t dz;
        uint8_t buttons;
        if (input_get_mouse(&dx, &dy, &dz, &buttons)) {
            const int mouse_joy_mode = config_get_mouse_joystick();
            static MouseScaleState mouse_scale = { .setting = -1 };
            if (config_get_mouse_invert_y())
                dy = -dy;
            if (mouse_joy_mode != MOUSE_JOYSTICK_DISABLED)
                mouse_joystick_event(dx, dy, buttons);
            mouse_scale_delta(&mouse_scale, &dx, &dy);
            if (config_get_mouse() && pc->mouse &&
                mouse_joy_mode != MOUSE_JOYSTICK_ONLY)
                ps2_mouse_event(pc->mouse, dx, dy, dz, buttons);
        }
    }

    /* Host mouse -> analog joystick translator.  A zero report centres it
     * immediately; mice that stop reporting after the last non-zero delta
     * are forced back to centre after 20 ms. */
    if (pc && !pc->paused &&
        config_get_mouse_joystick() != MOUSE_JOYSTICK_DISABLED) {
        if (!mouse_joystick_state.was_enabled) {
            mouse_joystick_state.x = 0;
            mouse_joystick_state.y = 0;
            mouse_joystick_state.buttons = 0;
            mouse_joystick_state.last_event_us = 0;
        }
        mouse_joystick_state.was_enabled = true;
        if (mouse_joystick_state.last_event_us &&
            (uint32_t)(time_us_32() - mouse_joystick_state.last_event_us) > 20000u) {
            mouse_joystick_state.x = 0;
            mouse_joystick_state.y = 0;
        }
        gameport_set_analog(mouse_joystick_state.x,
                            mouse_joystick_state.y,
                            mouse_joystick_state.buttons);
    } else {
        mouse_joystick_state.was_enabled = false;
    }

    numpad_mouse_tick();
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
    /* NULL main BIOS means the native BIOS generated by bios_post(). */
    config.bios = NULL;
    config.vga_bios = "vgabios.bin";
}

static int load_config_from_sd(void) {
    FIL fp;
    FRESULT res = f_open(&fp, CONFIG_FILE_PATH, FA_READ);
    if (res != FR_OK) {
        printf("Config file not found: %s (error %d)\n", CONFIG_FILE_PATH, res);
        return -1;
    }

    // Read entire file
    FSIZE_t size = f_size(&fp);
    char *content = malloc(size + 1);
    if (!content) {
        f_close(&fp);
        return -1;
    }

    UINT bytes_read;
    res = f_read(&fp, content, (UINT)size, &bytes_read);
    f_close(&fp);
    if (res != FR_OK) {
        free(content);
        return -1;
    }
    content[bytes_read] = '\0';

    // Parse INI content
    if (ini_parse_string(content, parse_conf_ini, &config) != 0) {
        printf("Failed to parse config\n");
        free(content);
        return -1;
    }

    // Also parse murm-286-specific settings
    ini_parse_string(content, parse_frank_386_ini, NULL);

    free(content);
    return 0;
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
    /* Same reachability problem as above: the settings-UI restart request
       was also only checked in the outer loop. */
    if (settingsui_restart_requested()) {
        settingsui_clear_restart();
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
        // Skip CPU execution when paused (disk UI or settings UI active)
        if (pc->paused) {
            // Still poll keyboard to handle UI input
            poll_keyboard();

            // Animate plasma background for active UI
            if (diskui_is_open()) {
                diskui_animate();
            } else if (settingsui_is_open()) {
                settingsui_animate();
            }
            if (settingsui_restart_requested()) {
                settingsui_clear_restart();
                hard_reboot();
            }
            const struct timespec frame = { .tv_sec = 0, .tv_nsec = 16000000 };
            nanosleep(&frame, NULL);  // ~60Hz polling/animation rate
            continue;
        }

        for (int i = 0; i < 10 && cpu_running && !pc->paused; i++)
            pc_step(pc, 4096);
    }
}

int main(int argc, char **argv)
{
    const char *device = argc > 1 ? argv[1] : "/dev/dri/card0";
    saved_argv = argv;
    load_default_config();
    if (load_config_from_sd() != 0)
        printf("Using default configuration\n");

    /* An emulator reboot re-executes the process with WARM_BOOT_ENV set
     * (platform_reboot()): only a fresh start runs the cold native POST. */
    const char *warm = getenv(WARM_BOOT_ENV);
    const bool cold_boot = !(warm && warm[0] == '1');
    unsetenv(WARM_BOOT_ENV);

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

    // Ensure emulator starts unpaused
    pc->paused = 0;

    // Initialize disk UI and settings UI
    diskui_init();
    settingsui_init();

    // Initialize config save module with current values from PCConfig
    config_set_cpu_gen(config.cpu_gen);
    config_set_fpu(config.fpu);
    config_set_bios_file(config.bios);
    // Re-apply Covox once so a saved Sound Master selection establishes its
    // I/O base and removes stale AdLib/SB conflicts from older configs.
    config_set_covox(config_get_covox());
    config_clear_changes();

    // Apply audio/mouse enable settings from config to PC instance
    pc->pcspk_enabled = config_get_pcspeaker();
    pc->adlib_enabled = config_get_adlib();
    pc->sb16_enabled = config_get_soundblaster();
    pc->tandy_enabled = config_get_tandy();
    pc->covox_enabled = config_get_covox();
    pc->mpu401_enabled = config_get_mpu401();
    pc->dss_enabled = config_get_dss();
    pc->mouse_enabled = config_get_mouse() || config_get_numpad_mouse();
    pc->joystick_enabled = config_get_mouse_joystick();
    gameport_set_button_swap(config_get_joystick_swap_buttons());
    printf("Audio: PC Speaker=%d, Adlib=%d, SB16=%d, MPU401=%d, Tandy=%d, Covox=%d, DSS=%d, Mouse=%d\n",
           pc->pcspk_enabled, pc->adlib_enabled, pc->sb16_enabled, pc->mpu401_enabled,
           pc->tandy_enabled, pc->covox_enabled, pc->dss_enabled, pc->mouse_enabled);

    // Select cold/warm native POST; bios_post() consumes the cold state
    // after the first native POST.
    pc_set_cold_post_pending(cold_boot);

    load_bios_and_reset(pc);

    // Audio comes up with the machine, like RP2350 core1 does
    audio_set_enabled(false);
    audio_init();
    audio_set_volume((uint8_t)config_get_volume());
    audio_set_enabled(true);

    // Native POST ran before the audio thread started; emit its queued tone.
    pc_play_pending_post_beep(pc);

    int result = EXIT_FAILURE;
    if (video_drm_open(device, VGA_HW_WIDTH, VGA_HW_HEIGHT) &&
        vga_hw_start(pc->vga)) {
        input_init();
        run_pc();
        result = EXIT_SUCCESS;
    }

    if (reboot_requested)
        platform_reboot();
    host_shutdown();
    return result;
}
