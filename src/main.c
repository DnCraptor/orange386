#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include "pc.h"
#include "platform_linux.h"
#include "vga_text.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

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
    config.vga_mem_size = 256 * 1024;
    config.cpu_gen = 3;
    config.fpu = 1;
    config.width = 720;
    config.height = 400;
    /* NULL main BIOS means the native BIOS, which is not ported yet. */
    config.bios = NULL;
    config.vga_bios = "vgabios.bin";
}

static void platform_poll(void *opaque)
{
    (void)opaque;
}

static int run_pc(PC *pc, uint8_t *pixels,
                  uint32_t pitch, uint32_t width, uint32_t height)
{
    void (*previous)(int) = signal(SIGINT, stop_cpu);
    puts("PC running; Ctrl+C stops emulation and restores DRM.");
    fflush(stdout);
    uint32_t last_frame = get_uticks();
    while (cpu_running && !pc->shutdown_state) {
        pc_step(pc, 4096);
        if (pc->reset_request) {
            pc->reset_request = 0;
            load_bios_and_reset(pc);
        }
        /* Bootstrap scanout at ~60 Hz until vga.c is ported. */
        uint32_t now = get_uticks();
        if ((uint32_t)(now - last_frame) >= 16667u) {
            last_frame = now;
            vga_text_render(pixels, pitch, width, height, guest_ram_base);
        }
    }
    signal(SIGINT, previous);
    return 0;
}

struct buffer {
    uint32_t handle, pitch, size, fb;
    void *pixels;
};

static void destroy_buffer(int fd, struct buffer *b)
{
    if (b->pixels && b->pixels != MAP_FAILED) munmap(b->pixels, b->size);
    if (b->fb) drmModeRmFB(fd, b->fb);
    if (b->handle) {
        struct drm_mode_destroy_dumb d = {.handle = b->handle};
        ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
    }
}

static int create_buffer(int fd, uint32_t width, uint32_t height, struct buffer *b)
{
    struct drm_mode_create_dumb c = {.width = width, .height = height, .bpp = 32};
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c) < 0) return -1;
    b->handle = c.handle;
    b->pitch = c.pitch;
    b->size = c.size;
    if (drmModeAddFB(fd, width, height, 24, 32, b->pitch, b->handle, &b->fb)) return -1;
    struct drm_mode_map_dumb m = {.handle = b->handle};
    if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m) < 0) return -1;
    b->pixels = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, m.offset);
    if (b->pixels == MAP_FAILED) { b->pixels = NULL; return -1; }
    return 0;
}

static void draw_bars(struct buffer *b, uint32_t width, uint32_t height)
{
    static const uint32_t colors[8] = {
        0x00ffffff, 0x00ffff00, 0x0000ffff, 0x0000ff00,
        0x00ff00ff, 0x00ff0000, 0x000000ff, 0x00000000
    };
    for (uint32_t y = 0; y < height; ++y) {
        uint32_t *line = (uint32_t *)((uint8_t *)b->pixels + (size_t)y * b->pitch);
        for (uint32_t x = 0; x < width; ++x)
            line[x] = colors[(uint64_t)x * 8 / width];
    }
}

static uint32_t find_crtc(int fd, drmModeRes *res, drmModeConnector *conn)
{
    if (conn->encoder_id) {
        drmModeEncoder *e = drmModeGetEncoder(fd, conn->encoder_id);
        if (e) {
            uint32_t id = e->crtc_id;
            drmModeFreeEncoder(e);
            if (id) return id;
        }
    }
    for (int i = 0; i < conn->count_encoders; ++i) {
        drmModeEncoder *e = drmModeGetEncoder(fd, conn->encoders[i]);
        if (!e) continue;
        for (int j = 0; j < res->count_crtcs; ++j) {
            if (e->possible_crtcs & (1u << j)) {
                uint32_t id = res->crtcs[j];
                drmModeFreeEncoder(e);
                return id;
            }
        }
        drmModeFreeEncoder(e);
    }
    return 0;
}

/*
 * Select a physical HDMI mode for an unscaled VGA raster.
 * The raster is always enlarged by an integer factor and centered;
 * unused pixels are black.  Never crop or downscale.
 *
 * This is deliberately independent of the VGA renderer: when the guest
 * changes video modes, call it again with the new source dimensions.
 */
struct video_placement {
    drmModeModeInfo mode;
    uint32_t scale, x, y, width, height;
};

static bool select_video_mode(const drmModeConnector *conn,
                              uint32_t src_w, uint32_t src_h,
                              struct video_placement *out)
{
    if (!src_w || !src_h) return false;
    bool found = false;
    uint64_t best_waste = UINT64_MAX;
    uint32_t best_scale = 0, best_refresh_error = UINT32_MAX;
    for (int i = 0; i < conn->count_modes; ++i) {
        const drmModeModeInfo *m = &conn->modes[i];
        uint32_t scale_w = m->hdisplay / src_w;
        uint32_t scale_h = m->vdisplay / src_h;
        uint32_t scale = scale_w < scale_h ? scale_w : scale_h;
        if (!scale) continue;

        uint32_t w = src_w * scale, h = src_h * scale;
        uint64_t area = (uint64_t)m->hdisplay * m->vdisplay;
        uint64_t waste = area - (uint64_t)w * h;
        /*
         * Compare unused-area fractions, not absolute pixel counts.
         * An exact match wins; among equally filled modes, prefer the
         * larger integer scale.  Prefer refresh close to 60 Hz last.
         */
        uint32_t refresh = m->vrefresh;
        uint32_t refresh_error = refresh > 60 ? refresh - 60 : 60 - refresh;
        bool better = !found ||
            waste * (uint64_t)out->mode.hdisplay * out->mode.vdisplay <
                best_waste * area ||
            (waste * (uint64_t)out->mode.hdisplay * out->mode.vdisplay ==
                best_waste * area &&
             (scale > best_scale ||
              (scale == best_scale && refresh_error < best_refresh_error)));
        if (!better) continue;
        out->mode = *m;
        out->scale = scale;
        out->width = w;
        out->height = h;
        out->x = (m->hdisplay - w) / 2;
        out->y = (m->vdisplay - h) / 2;
        best_waste = waste;
        best_scale = scale;
        best_refresh_error = refresh_error;
        found = true;
    }
    return found;
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
    int fd = open(device, O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror(device); return EXIT_FAILURE; }

    if (!platform_alloc_guest_ram((size_t)config.mem_size)) {
        perror("guest RAM allocation");
        close(fd);
        return EXIT_FAILURE;
    }

    PC *pc = pc_new(NULL, platform_poll, NULL, NULL, &config);
    if (!pc || !pc->cpu) {
        fprintf(stderr, "Cannot initialize PC\n");
        platform_free_guest_ram();
        close(fd);
        return EXIT_FAILURE;
    }
    load_bios_and_reset(pc);

    int result = EXIT_FAILURE;
    drmModeRes *res = drmModeGetResources(fd);
    drmModeConnector *conn = NULL;
    drmModeCrtc *old = NULL;
    struct buffer b = {0};
    uint32_t crtc_id = 0;
    bool changed = false;

    if (!res) { fprintf(stderr, "drmModeGetResources: %s\n", strerror(errno)); goto done; }
    for (int i = 0; i < res->count_connectors; ++i) {
        drmModeConnector *candidate = drmModeGetConnector(fd, res->connectors[i]);
        if (!candidate) continue;
        if (candidate->connection == DRM_MODE_CONNECTED && candidate->count_modes > 0 &&
            candidate->connector_type == DRM_MODE_CONNECTOR_HDMIA) {
            conn = candidate;
            break;
        }
        drmModeFreeConnector(candidate);
    }
    if (!conn) { fprintf(stderr, "No connected HDMI-A connector with modes found\n"); goto done; }

    printf("Available HDMI modes (%d):\n", conn->count_modes);
    for (int i = 0; i < conn->count_modes; ++i) {
        const drmModeModeInfo *m = &conn->modes[i];
        printf("  %4d x %-4d @ %6.2f Hz  %-16s%s\n",
            m->hdisplay,
            m->vdisplay,
            (double)m->clock * 1000.0 /
                (m->htotal * m->vtotal),
            m->name,
            (m->type & DRM_MODE_TYPE_PREFERRED) ? " [preferred]" : "");
    }

    crtc_id = find_crtc(fd, res, conn);
    if (!crtc_id) { fprintf(stderr, "No usable CRTC found\n"); goto done; }
    old = drmModeGetCrtc(fd, crtc_id);
    if (!old) { fprintf(stderr, "Cannot read original CRTC\n"); goto done; }

    /* Initial VGA text raster (80 columns x 9 dots, 25 rows x 16 dots).
     * The VGA renderer will update this when the guest changes modes. */
    struct video_placement placement;
    if (!select_video_mode(conn, 720, 400, &placement)) {
        fprintf(stderr, "No HDMI mode fits the 720x400 VGA raster\n");
        goto done;
    }
    drmModeModeInfo mode = placement.mode;
    printf("VGA placement: 720x400 -> %ux%u, scale %ux, offset (%u,%u)\n",
           placement.width, placement.height, placement.scale,
           placement.x, placement.y);

    if (create_buffer(fd, mode.hdisplay, mode.vdisplay, &b)) {
        fprintf(stderr, "Cannot create dumb framebuffer: %s\n", strerror(errno)); goto done;
    }
    draw_bars(&b, mode.hdisplay, mode.vdisplay);
    if (drmModeSetCrtc(fd, crtc_id, b.fb, 0, 0, &conn->connector_id, 1, &mode)) {
        fprintf(stderr, "drmModeSetCrtc: %s\n", strerror(errno)); goto done;
    }
    changed = true;
    printf("DRM: %s, HDMI connector %u, %ux%u @ %u Hz\n", device,
           conn->connector_id, mode.hdisplay, mode.vdisplay, mode.vrefresh);
    puts("VGA text scanout enabled (bootstrap: B8000, 80x25 only).");
    result = run_pc(pc, b.pixels, b.pitch,
                    mode.hdisplay, mode.vdisplay) == 0
                    ? EXIT_SUCCESS : EXIT_FAILURE;

done:
    if (changed) {
        if (old->mode_valid) {
            if (drmModeSetCrtc(fd, old->crtc_id, old->buffer_id, old->x, old->y,
                               &conn->connector_id, 1, &old->mode))
                fprintf(stderr, "Warning: failed to restore CRTC: %s\n", strerror(errno));
        } else {
            if (drmModeSetCrtc(fd, old->crtc_id, 0, 0, 0, NULL, 0, NULL))
                fprintf(stderr, "Warning: failed to disable CRTC: %s\n", strerror(errno));
        }
    }
    destroy_buffer(fd, &b);
    if (old) drmModeFreeCrtc(old);
    if (conn) drmModeFreeConnector(conn);
    if (res) drmModeFreeResources(res);
    platform_free_guest_ram();
    close(fd);
    return result;
}
