#ifndef IMGFS_H
#define IMGFS_H

/*
 * FAT volumes inside the attached disk images, for the FTP server.
 *
 * Physical drive N of this FatFs copy is a host file descriptor set with
 * imgfs_set_drive(): a disk image (with or without MBR) or the raw_disk=
 * device.  The descriptor is used with pread()/pwrite() only, so it may be a
 * dup() of the emulator's own descriptor for that image.
 */
#include "imgfs_names.h"
#include "ff.h"

#ifdef __cplusplus
extern "C" {
#endif

/* fd < 0 removes the drive.  readonly makes the volume refuse writes. */
void imgfs_set_drive(BYTE pdrv, int fd, int readonly);

#ifdef __cplusplus
}
#endif

#endif /* IMGFS_H */
