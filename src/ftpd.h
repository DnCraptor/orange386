#ifndef ORANGE386_FTPD_H
#define ORANGE386_FTPD_H

#include <stdbool.h>
#include <stddef.h>

/*
 * FTP access to the FAT volumes inside the attached disk images.
 *
 * Linux counterpart of murm386's USB DEVICE mode: the server runs only while
 * its OSD page (ftpui.c) keeps the guest paused, and closing that page
 * reboots the emulated PC, so the guest never sees a volume change under its
 * own caches.  One client is served at a time; FAT volumes appear as
 * top-level directories named after their Disk Manager rows.
 */

#define FTPD_MAX_VOLUMES 7

typedef struct {
    const char *name;  /* top-level directory, e.g. "FDD-0" */
    int fd;            /* host descriptor of the image; ftpd dup()s it */
    bool readonly;
} ftpd_drive;

/* Mount the drives and start serving on TCP port.  false and a message in
 * err when nothing could be started (bind failed, no FAT volume found). */
bool ftpd_start(const ftpd_drive *drives, int count, unsigned port,
                const char *user, const char *password,
                char *err, size_t err_size);

/* Stop serving, flush and unmount every volume.  Idempotent. */
void ftpd_stop(void);

/* Volumes as given to ftpd_start(): name and whether a FAT volume mounted. */
int ftpd_volume_count(void);
bool ftpd_volume(int index, const char **name, bool *mounted);

/* One line of current activity for the OSD page. */
void ftpd_status(char *buf, size_t size);

#endif /* ORANGE386_FTPD_H */
