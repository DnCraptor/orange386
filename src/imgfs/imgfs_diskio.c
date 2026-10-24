/*
 * Low level disk I/O for the image FatFs (src/imgfs): physical drive N is a
 * host file descriptor (see imgfs.h).
 */
#include "imgfs.h"
#include "diskio.h"

#include <errno.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <time.h>
#include <unistd.h>

/* fd is stored + 1 so that the zero-initialized table means "no drive". */
static struct {
	int fd_plus1;
	int readonly;
} slots[FF_VOLUMES];

void imgfs_set_drive(BYTE pdrv, int fd, int readonly)
{
	if (pdrv >= FF_VOLUMES)
		return;
	slots[pdrv].fd_plus1 = fd < 0 ? 0 : fd + 1;
	slots[pdrv].readonly = readonly;
}

static int drive_fd(BYTE pdrv)
{
	return pdrv < FF_VOLUMES ? slots[pdrv].fd_plus1 - 1 : -1;
}

static uint64_t drive_sectors(int fd)
{
	struct stat st;
	uint64_t bytes = 0;
	if (fstat(fd, &st) != 0)
		return 0;
	if (S_ISBLK(st.st_mode)) {
		if (ioctl(fd, BLKGETSIZE64, &bytes) != 0)
			bytes = 0;
	} else {
		bytes = (uint64_t)st.st_size;
	}
	return bytes / 512u;
}

DSTATUS disk_status(BYTE pdrv)
{
	if (drive_fd(pdrv) < 0)
		return STA_NOINIT | STA_NODISK;
	return slots[pdrv].readonly ? STA_PROTECT : 0;
}

DSTATUS disk_initialize(BYTE pdrv)
{
	return disk_status(pdrv);
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
	const int fd = drive_fd(pdrv);
	if (fd < 0)
		return RES_NOTRDY;
	size_t len = (size_t)count * 512u;
	off_t off = (off_t)sector * 512;
	while (len) {
		ssize_t n = pread(fd, buff, len, off);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return RES_ERROR;
		buff += n;
		off += n;
		len -= (size_t)n;
	}
	return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
	const int fd = drive_fd(pdrv);
	if (fd < 0)
		return RES_NOTRDY;
	if (slots[pdrv].readonly)
		return RES_WRPRT;
	size_t len = (size_t)count * 512u;
	off_t off = (off_t)sector * 512;
	while (len) {
		ssize_t n = pwrite(fd, buff, len, off);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return RES_ERROR;
		buff += n;
		off += n;
		len -= (size_t)n;
	}
	return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
	const int fd = drive_fd(pdrv);
	if (fd < 0)
		return RES_NOTRDY;
	switch (cmd) {
	case CTRL_SYNC:
		return fdatasync(fd) == 0 ? RES_OK : RES_ERROR;
	case GET_SECTOR_COUNT: {
		uint64_t sectors = drive_sectors(fd);
		if (sectors > 0xFFFFFFFFu)
			sectors = 0xFFFFFFFFu;
		*(LBA_t *)buff = (LBA_t)sectors;
		return RES_OK;
	}
	case GET_SECTOR_SIZE:
		*(WORD *)buff = 512;
		return RES_OK;
	case GET_BLOCK_SIZE:
		*(DWORD *)buff = 1;
		return RES_OK;
	default:
		return RES_PARERR;
	}
}

/* Timestamps of files written over FTP: host local time. */
DWORD get_fattime(void)
{
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	if (tm.tm_year < 80)
		return (DWORD)((0u << 25) | (1u << 21) | (1u << 16));
	return (DWORD)((unsigned)(tm.tm_year - 80) << 25 |
		       (unsigned)(tm.tm_mon + 1) << 21 |
		       (unsigned)tm.tm_mday << 16 |
		       (unsigned)tm.tm_hour << 11 |
		       (unsigned)tm.tm_min << 5 |
		       (unsigned)tm.tm_sec >> 1);
}
