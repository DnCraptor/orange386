#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include "i386.h"
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

/* Initial Linux backing store for the guest physical address space. */
#define GUEST_RAM_SIZE (16u * 1024u * 1024u)
uint8_t *guest_ram_base;

/* Linux host port: initial CPU I/O bus, pending device implementations. */
static volatile sig_atomic_t cpu_running = 1;

static void stop_cpu(int signum)
{
    (void)signum;
    cpu_running = 0;
}

static uint8_t io_read8(void *opaque, int port)
{
    (void)opaque;
    (void)port;
    return 0xff;
}

static uint16_t io_read16(void *opaque, int port)
{
    uint16_t lo = io_read8(opaque, port);
    return lo | ((uint16_t)io_read8(opaque, port + 1) << 8);
}

static uint32_t io_read32(void *opaque, int port)
{
    uint32_t lo = io_read16(opaque, port);
    return lo | ((uint32_t)io_read16(opaque, port + 2) << 16);
}

static void io_write8(void *opaque, int port, uint8_t value)
{
    (void)opaque;
    (void)port;
    (void)value;
}

static void io_write16(void *opaque, int port, uint16_t value)
{
    io_write8(opaque, port, (uint8_t)value);
    io_write8(opaque, port + 1, (uint8_t)(value >> 8));
}

static void io_write32(void *opaque, int port, uint32_t value)
{
    io_write16(opaque, port, (uint16_t)value);
    io_write16(opaque, port + 2, (uint16_t)(value >> 16));
}

static int io_read_string(void *opaque, int port, uint32_t addr, int dir, int count)
{
    (void)opaque; (void)port; (void)addr; (void)dir; (void)count;
    return 0; /* CPU handles each transfer using io_read8/16/32. */
}

static int io_write_string(void *opaque, int port, uint32_t addr, int size, int count)
{
    (void)opaque; (void)port; (void)addr; (void)size; (void)count;
    return 0;
}

static int pic_read_irq(void *opaque)
{
    (void)opaque;
    return 0xff; /* Called only after a device raises an IRQ. */
}

static int load_bios_rom(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        perror(path);
        return -1;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        perror("BIOS seek");
        fclose(file);
        return -1;
    }
    long length = ftell(file);
    if (length < 16 || length > 0x10000 || fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "BIOS image must be 16..65536 bytes: %s\\n", path);
        fclose(file);
        return -1;
    }
    /* ROMs smaller than 64 KiB are mapped at the top of the first MiB. */
    memset(guest_ram_base + 0xf0000, 0xff, 0x10000);
    size_t offset = 0x100000u - (size_t)length;
    size_t n = fread(guest_ram_base + offset, 1, (size_t)length, file);
    fclose(file);
    if (n != (size_t)length) {
        fprintf(stderr, "Short BIOS read: %s\\n", path);
        return -1;
    }
    return 0;
}

static int run_cpu(const char *bios_path)
{
    CPU_CB *cb = NULL;
    CPU *cpu = cpu_new(3, &cb);
    if (!cpu || !cb) {
        fprintf(stderr, "Cannot initialize i386 CPU\\n");
        return -1;
    }
    cb->io_read8 = io_read8;
    cb->io_read16 = io_read16;
    cb->io_read32 = io_read32;
    cb->io_write8 = io_write8;
    cb->io_write16 = io_write16;
    cb->io_write32 = io_write32;
    cb->io_read_string = io_read_string;
    cb->io_write_string = io_write_string;
    cb->pic_read_irq = pic_read_irq;
    cpu->bios = bios_path; /* Execute ROM instructions, not native BIOS stubs. */
    cpu_reset(cpu);

    void (*previous)(int) = signal(SIGINT, stop_cpu);
    puts("i386 CPU running; Ctrl+C stops emulation and restores DRM.");
    fflush(stdout);
    while (cpu_running)
        cpu_step(cpu, 2048);
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

int main(int argc, char **argv)
{
    const char *bios_path = NULL;
    const char *device = "/dev/dri/card0";
    if (argc > 1 && strcmp(argv[1], "--bios") == 0) {
        if (argc < 3 || argc > 4) {
            fprintf(stderr, "Usage: %s --bios rom.bin [/dev/dri/cardN]\\n", argv[0]);
            return EXIT_FAILURE;
        }
        bios_path = argv[2];
        if (argc == 4) device = argv[3];
    } else if (argc > 1) {
        device = argv[1];
    }
    int fd = open(device, O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror(device); return EXIT_FAILURE; }

    guest_ram_base = calloc(1, GUEST_RAM_SIZE);
    if (!guest_ram_base) {
        perror("guest RAM allocation");
        close(fd);
        return EXIT_FAILURE;
    }

    if (bios_path && load_bios_rom(bios_path) != 0) {
        free(guest_ram_base);
        guest_ram_base = NULL;
        close(fd);
        return EXIT_FAILURE;
    }

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

    drmModeModeInfo mode = conn->modes[0];
    for (int i = 0; i < conn->count_modes; ++i)
        if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) { mode = conn->modes[i]; break; }

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
    if (bios_path) {
        puts("DRM framebuffer is provisional; VGA scanout is not connected yet.");
        result = run_cpu(bios_path) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    } else {
        puts("Eight color bars are displayed. Press Enter to restore the display.");
        fflush(stdout);
        (void)getchar();
        result = EXIT_SUCCESS;
    }

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
    free(guest_ram_base);
    guest_ram_base = NULL;
    close(fd);
    return result;
}
