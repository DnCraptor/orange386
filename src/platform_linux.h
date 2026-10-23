#ifndef ORANGE386_PLATFORM_LINUX_H
#define ORANGE386_PLATFORM_LINUX_H

#include <stddef.h>
#include <stdint.h>

/* Reserve the guest physical address space; returns guest_ram_base. */
uint8_t *platform_alloc_guest_ram(size_t mem_size);
void platform_free_guest_ram(void);

/* Host identity for the native BIOS banner and POST table. */
void platform_host_name(char *buf, size_t size);
unsigned platform_cpu_mhz(void);        /* 0 if unknown */
unsigned long platform_host_ram_mb(void); /* 0 if unknown */

#endif /* ORANGE386_PLATFORM_LINUX_H */
