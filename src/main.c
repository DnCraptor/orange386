#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pc.h"
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
}

static void run_pc(PC *pc)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = stop_cpu;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    puts("PC running; Ctrl+C stops emulation and restores DRM.");
    fflush(stdout);
    while (cpu_running && !pc->shutdown_state) {
        for (int i = 0; i < 10; i++)
            pc_step(pc, 4096);
        if (pc->reset_request) {
            pc->reset_request = 0;
            load_bios_and_reset(pc);
        }
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

    PC *pc = pc_new(vga_redraw, platform_poll, NULL, NULL, &config);
    if (!pc || !pc->cpu) {
        fprintf(stderr, "Cannot initialize PC\n");
        platform_free_guest_ram();
        return EXIT_FAILURE;
    }
    load_bios_and_reset(pc);

    int result = EXIT_FAILURE;
    if (video_drm_open(device, VGA_HW_WIDTH, VGA_HW_HEIGHT)) {
        if (vga_hw_start(pc->vga)) {
            run_pc(pc);
            vga_hw_stop();
            result = EXIT_SUCCESS;
        }
        video_drm_close();
    }

    platform_free_guest_ram();
    return result;
}
