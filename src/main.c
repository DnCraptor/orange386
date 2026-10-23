#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
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
    const char *device = argc > 1 ? argv[1] : "/dev/dri/card0";
    int fd = open(device, O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror(device); return EXIT_FAILURE; }

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
    puts("Eight color bars are displayed. Press Enter to restore the display.");
    fflush(stdout);
    (void)getchar();
    result = EXIT_SUCCESS;

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
    close(fd);
    return result;
}
