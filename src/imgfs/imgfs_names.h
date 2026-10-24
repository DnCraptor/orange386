/*
 * The FTP server's FatFs (src/imgfs) is a second, unmodified copy of ChaN's
 * FatFs next to the POSIX FatFs API layer of the emulator (src/ff.h,
 * ff_posix.c).  Its external names get an imgfs_ prefix so both can be linked.
 * Include this before imgfs/ff.h, and only in translation units that use
 * this copy.
 */
#ifndef IMGFS_NAMES_H
#define IMGFS_NAMES_H

#define f_open          imgfs_f_open
#define f_close         imgfs_f_close
#define f_read          imgfs_f_read
#define f_write         imgfs_f_write
#define f_lseek         imgfs_f_lseek
#define f_truncate      imgfs_f_truncate
#define f_sync          imgfs_f_sync
#define f_opendir       imgfs_f_opendir
#define f_closedir      imgfs_f_closedir
#define f_readdir       imgfs_f_readdir
#define f_mkdir         imgfs_f_mkdir
#define f_unlink        imgfs_f_unlink
#define f_rename        imgfs_f_rename
#define f_stat          imgfs_f_stat
#define f_getfree       imgfs_f_getfree
#define f_mount         imgfs_f_mount
#define ff_oem2uni      imgfs_ff_oem2uni
#define ff_uni2oem      imgfs_ff_uni2oem
#define ff_wtoupper     imgfs_ff_wtoupper
#define disk_initialize imgfs_disk_initialize
#define disk_status     imgfs_disk_status
#define disk_read       imgfs_disk_read
#define disk_write      imgfs_disk_write
#define disk_ioctl      imgfs_disk_ioctl
#define get_fattime     imgfs_get_fattime

#endif /* IMGFS_NAMES_H */
