#ifndef ORANGE386_PLATFORM_LINUX_H
#define ORANGE386_PLATFORM_LINUX_H

#include <stddef.h>
#include <stdint.h>

/* Reserve the guest physical address space; returns guest_ram_base. */
uint8_t *platform_alloc_guest_ram(size_t mem_size);
void platform_free_guest_ram(void);

#endif /* ORANGE386_PLATFORM_LINUX_H */
