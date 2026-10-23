#ifndef ORANGE386_SDCARD_H
#define ORANGE386_SDCARD_H

#include <stddef.h>

/*
 * murm386 drivers/sdcard/sdcard.h: the native DOS kernel lends spare guest
 * memory to the RP2350 FatFs sector caches.  On the Linux host FatFs is the
 * POSIX layer (ff_posix.c) and the kernel page cache does that job, so the
 * cache hooks accept the memory and ignore it.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SDCARD_FF_QSPI_OWNER_XMS = 0,
    SDCARD_FF_QSPI_OWNER_NATIVE = 1,
    SDCARD_FF_QSPI_OWNER_COUNT
} sdcard_ff_qspi_owner_t;

static inline void sdcard_enable_ff_dos_cache(void *storage, size_t bytes)
{
    (void)storage;
    (void)bytes;
}

static inline void sdcard_enable_ff_qspi_cache(void *minimum, void *ceiling)
{
    (void)minimum;
    (void)ceiling;
}

static inline void sdcard_ff_qspi_cache_set_floor(sdcard_ff_qspi_owner_t owner,
                                                  void *floor)
{
    (void)owner;
    (void)floor;
}

#ifdef __cplusplus
}
#endif

#endif /* ORANGE386_SDCARD_H */
