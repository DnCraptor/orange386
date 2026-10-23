#ifndef USBMSC_HOST_H
#define USBMSC_HOST_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Linux host replacement for murm386 drivers/usbhid/usbmsc_host.h.
 *
 * RP2350 exposes a boot-detected USB mass-storage stick as an extra BIOS hard
 * disk through its TinyUSB MSC host stack.  On Linux the kernel owns USB
 * storage: no stick is ever reported here, so disk.c never enables its
 * raw-USB HDD (disk_set_raw_usb_hdd() is not called).
 */
static inline bool usbmsc_host_ready(void) { return false; }
static inline bool usbmsc_host_wait_ready(uint32_t timeout_ms) { (void)timeout_ms; return false; }
static inline uint32_t usbmsc_host_sector_count(void) { return 0; }
static inline bool usbmsc_host_read(uint32_t lba, void *buf, uint16_t count)
{ (void)lba; (void)buf; (void)count; return false; }
static inline bool usbmsc_host_write(uint32_t lba, const void *buf, uint16_t count)
{ (void)lba; (void)buf; (void)count; return false; }
static inline bool usbmsc_host_sync(void) { return false; }

#endif /* USBMSC_HOST_H */
