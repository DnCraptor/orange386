#include "mem.h"
#include <stdint.h>
#include <string.h>

/* Temporary memory-backed I/O aperture, not a VGA/device implementation.
 * g_pc and phys_mem_size are owned by pc.c. */

static int valid(uint32_t addr, uint32_t len)
{
    return guest_ram_base && addr < phys_mem_size && len <= phys_mem_size - addr;
}

uint8_t iomem_read8(void *iomem, uint32_t addr)
{
    (void)iomem;
    return valid(addr, 1) ? PC_RAM[addr] : 0xff;
}

uint16_t iomem_read16(void *iomem, uint32_t addr)
{
    return (uint16_t)iomem_read8(iomem, addr) |
           ((uint16_t)iomem_read8(iomem, addr + 1) << 8);
}

uint32_t iomem_read32(void *iomem, uint32_t addr)
{
    return (uint32_t)iomem_read16(iomem, addr) |
           ((uint32_t)iomem_read16(iomem, addr + 2) << 16);
}

void iomem_write8(void *iomem, uint32_t addr, uint8_t val)
{
    (void)iomem;
    if (valid(addr, 1)) PC_RAM[addr] = val;
}

void iomem_write16(void *iomem, uint32_t addr, uint16_t val)
{
    iomem_write8(iomem, addr, (uint8_t)val);
    iomem_write8(iomem, addr + 1, (uint8_t)(val >> 8));
}

void iomem_write32(void *iomem, uint32_t addr, uint32_t val)
{
    iomem_write16(iomem, addr, (uint16_t)val);
    iomem_write16(iomem, addr + 2, (uint16_t)(val >> 16));
}

bool iomem_write_string_ptr(void *iomem, uint32_t addr, const uint8_t *buf, int len)
{
    (void)iomem;
    if (len < 0 || !buf || !valid(addr, (uint32_t)len)) return false;
    memcpy(PC_RAM + addr, buf, (size_t)len);
    return true;
}

bool iomem_write_string(void *iomem, uint32_t addr, uint32_t buf, int len)
{
    if (!valid(buf, len < 0 ? 0 : (uint32_t)len)) return false;
    return iomem_write_string_ptr(iomem, addr, PC_RAM + buf, len);
}
