/*
 * Win+F10 OSD page: FTP server for the disk images (see ftpui.h, ftpd.h).
 */
#include "ftpui.h"
#include "ftpd.h"
#include "vga_osd.h"
#include "config_save.h"
#include "disk.h"
#include "diskui.h"
#include "ff.h"
#include "diskio.h"
#include <hardware/watchdog.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>

#define FTPUI_X 6
#define FTPUI_Y 4
#define FTPUI_W 68
#define FTPUI_H 17

static bool open_now;
static bool server_ok;
static char start_error[64];
static int plasma_frame;
static char last_status[96];

/* Disk Manager row names, which are also the FTP top-level directories. */
static const char *const fdd_names[2] = { "FDD-0", "FDD-1" };
static const char *const ata_names[4] = { "ATA0-0", "ATA0-1", "ATA1-0", "ATA1-1" };

static int collect_drives(ftpd_drive *out)
{
    int n = 0;
    for (uint8_t i = 0; i < 2; ++i) {
        FIL *f = fdd_get_file(i);
        if (fdd_is_inserted(i) && f && f->fd >= 0)
            out[n++] = (ftpd_drive){ fdd_names[i], f->fd, false };
    }
    for (uint8_t i = 0; i < 4; ++i) {
        FIL *f = ata_get_file(i);
        if (ata_is_inserted(i) && !ata_is_cdrom(i) && f && f->fd >= 0)
            out[n++] = (ftpd_drive){ ata_names[i], f->fd, false };
    }
    if (disk_raw_fd() >= 0)
        out[n++] = (ftpd_drive){ "RAWDISK", disk_raw_fd(),
                                 (disk_status(0) & STA_PROTECT) != 0 };
    return n;
}

/* Up to three "ftp://a.b.c.d:port/" lines from the host's IPv4 interfaces. */
static int host_urls(char urls[3][40], unsigned port)
{
    struct ifaddrs *list = NULL;
    int n = 0;
    if (getifaddrs(&list) != 0)
        return 0;
    for (struct ifaddrs *ifa = list; ifa && n < 3; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET ||
            !(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK))
            continue;
        char ip[INET_ADDRSTRLEN];
        const struct sockaddr_in *sin = (const struct sockaddr_in *)ifa->ifa_addr;
        if (!inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip)))
            continue;
        snprintf(urls[n++], 40, "ftp://%s:%u/", ip, port);
    }
    freeifaddrs(list);
    return n;
}

static void draw_status_line(void)
{
    const uint8_t attr = OSD_ATTR_NORMAL;
    char line[FTPUI_W - 4 + 1];
    char status[96];
    ftpd_status(status, sizeof(status));
    snprintf(last_status, sizeof(last_status), "%s", status);
    snprintf(line, sizeof(line), "Status:   %-54.54s", status);
    osd_print(FTPUI_X + 2, FTPUI_Y + 12, line, attr);
}

static void draw_page(void)
{
    const uint8_t attr = OSD_ATTR_NORMAL;
    const uint8_t hl = OSD_ATTR_HIGHLIGHT;
    char line[FTPUI_W];

    osd_fill(FTPUI_X, FTPUI_Y, FTPUI_W, FTPUI_H, ' ', attr);
    osd_draw_box_titled(FTPUI_X, FTPUI_Y, FTPUI_W, FTPUI_H, " FTP server ", OSD_ATTR_BORDER);

    if (!server_ok) {
        osd_print(FTPUI_X + 3, FTPUI_Y + 3, "The FTP server could not be started:", attr);
        osd_print(FTPUI_X + 3, FTPUI_Y + 5, start_error, hl);
        osd_print_center(FTPUI_Y + FTPUI_H - 2, "Esc: back to the PC", hl);
        return;
    }

    const unsigned port = (unsigned)config_get_ftp_port();
    char urls[3][40];
    int nurl = host_urls(urls, port);
    if (!nurl) {
        snprintf(line, sizeof(line), "Address:  port %u (no network interface is up)", port);
        osd_print(FTPUI_X + 2, FTPUI_Y + 2, line, attr);
    }
    for (int i = 0; i < nurl; ++i) {
        snprintf(line, sizeof(line), "%s%.39s", i ? "          " : "Address:  ", urls[i]);
        osd_print(FTPUI_X + 2, FTPUI_Y + 2 + i, line, i ? attr : hl);
    }
    snprintf(line, sizeof(line), "User:     %.20s     Password: %.20s",
             config_get_ftp_user(), config_get_ftp_password());
    osd_print(FTPUI_X + 2, FTPUI_Y + 6, line, attr);

    osd_print(FTPUI_X + 2, FTPUI_Y + 8, "Volumes:", attr);
    int col = 0, row = 0;
    for (int i = 0; i < ftpd_volume_count(); ++i) {
        const char *name;
        bool mounted;
        if (!ftpd_volume(i, &name, &mounted))
            continue;
        snprintf(line, sizeof(line), "/%.15s%s", name, mounted ? "" : " (no FAT)");
        osd_print(FTPUI_X + 12 + col * 18, FTPUI_Y + 8 + row, line,
                  mounted ? attr : OSD_ATTR_DISABLED);
        if (++col == 3) {
            col = 0;
            ++row;
        }
    }

    draw_status_line();
    osd_print_center(FTPUI_Y + FTPUI_H - 3,
                     "The PC is paused while the server runs.", attr);
    osd_print_center(FTPUI_Y + FTPUI_H - 2,
                     "Esc: stop the server and restart the PC", hl);
}

bool ftpui_is_open(void)
{
    return open_now;
}

void ftpui_open(void)
{
    if (open_now)
        return;
    ftpd_drive drives[FTPD_MAX_VOLUMES];
    int n = collect_drives(drives);
    if (!n) {
        snprintf(start_error, sizeof(start_error), "No disk image is attached");
        server_ok = false;
    } else {
        server_ok = ftpd_start(drives, n, (unsigned)config_get_ftp_port(),
                               config_get_ftp_user(), config_get_ftp_password(),
                               start_error, sizeof(start_error));
    }
    open_now = true;
    plasma_frame = 0;
    last_status[0] = 0;
    osd_clear();
    osd_show();
    draw_page();
}

void ftpui_handle_key(int keycode, bool is_down)
{
    if (!open_now || !is_down || keycode != KEY_ESC)
        return;
    if (!server_ok) {
        /* Nothing was exported: just resume the guest. */
        open_now = false;
        osd_hide();
        return;
    }
    /* Images may have changed under the guest's caches: restart the PC. */
    ftpd_stop();
    watchdog_reboot(0, 0, 0);
}

void ftpui_animate(void)
{
    if (!open_now)
        return;
    plasma_frame++;
    osd_draw_plasma_background(plasma_frame * 3, FTPUI_X, FTPUI_Y, FTPUI_W, FTPUI_H);
    if ((plasma_frame & 7) == 0 && server_ok) {
        char status[96];
        ftpd_status(status, sizeof(status));
        if (strcmp(status, last_status) != 0)
            draw_status_line();
    }
}
