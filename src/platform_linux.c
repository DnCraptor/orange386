#include "pc.h"
#include "mem.h"
#include "platform_linux.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

/*
 * Linux/ARM64 platform HAL: the counterpart of the HAL functions that
 * murm386 implements in main.c / platform_rp2350.c.
 */

uint8_t *guest_ram_base;
static size_t guest_ram_mapping;

/*
 * Guest physical memory.
 *
 * The CPU core addresses PC_RAM + addr without bounds checks
 * (CHECK_RAM_BOARDER_ENABLED=0): on RP2350 any 32-bit physical address
 * lands somewhere inside the PSRAM window.  Reserve the whole 4 GiB guest
 * physical space (plus one page for accesses straddling the top) so the same
 * holds here.  The reservation is lazily backed: only pages the guest touches
 * consume host memory, so RAM beyond conf->mem_size simply reads as zero.
 */
uint8_t *platform_alloc_guest_ram(size_t mem_size)
{
	const size_t full = ((size_t)1 << 32) + 4096u;
	void *p = mmap(NULL, full, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (p != MAP_FAILED) {
		guest_ram_mapping = full;
	} else {
		/* Strict overcommit: fall back to the configured size plus the
		 * ROM/VGA area. Out-of-range guest addresses are then unsafe. */
		fprintf(stderr, "Warning: cannot reserve 4 GiB guest space (%s), "
			"using %zu bytes\n", strerror(errno), mem_size);
		guest_ram_mapping = mem_size + 4096u;
		p = mmap(NULL, guest_ram_mapping, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (p == MAP_FAILED) {
			guest_ram_mapping = 0;
			return NULL;
		}
	}
	guest_ram_base = p;
	return guest_ram_base;
}

void platform_free_guest_ram(void)
{
	if (guest_ram_base && guest_ram_mapping)
		munmap(guest_ram_base, guest_ram_mapping);
	guest_ram_base = NULL;
	guest_ram_mapping = 0;
}

/**
 * Get microsecond timestamp (wraps every ~71 minutes, like time_us_32()).
 */
uint32_t get_uticks(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)((uint64_t)ts.tv_sec * 1000000u +
			  (uint64_t)ts.tv_nsec / 1000u);
}

void *pcmalloc(long size)
{
	return malloc(size);
}

/**
 * Load ROM file from the data directory into guest memory.
 * backward != 0: the image ends at addr (system BIOS below 1 MiB).
 * Returns the image size, or -1.
 */
int load_rom(void *phys_mem, const char *file, uword addr, int backward)
{
	char path[1024];
	if (strchr(file, '/') || strchr(file, '\\'))
		snprintf(path, sizeof(path), "%s", file);
	else
		snprintf(path, sizeof(path), SD_DATA_DIR_SLASH "%s", file);

	FILE *fp = fopen(path, "rb");
	if (!fp) {
		printf("Failed to open ROM: %s (%s)\n", path, strerror(errno));
		return -1;
	}
	if (fseek(fp, 0, SEEK_END) != 0) {
		fclose(fp);
		return -1;
	}
	long size = ftell(fp);
	if (size <= 0 || size > 0x100000 || (backward && size > (long)addr) ||
	    fseek(fp, 0, SEEK_SET) != 0) {
		printf("ERROR: invalid ROM size: %s (%ld bytes)\n", path, size);
		fclose(fp);
		return -1;
	}

	uint8_t *dest = backward ? (uint8_t *)phys_mem + addr - size
				 : (uint8_t *)phys_mem + addr;
	size_t n = fread(dest, 1, (size_t)size, fp);
	fclose(fp);
	if (n != (size_t)size) {
		printf("ERROR: Failed to read ROM: %s (read %zu of %ld)\n",
		       path, n, size);
		return -1;
	}
	printf("Loaded ROM: %s (%ld bytes) at 0x%05lx\n", path, size,
	       (unsigned long)(backward ? addr - size : addr));
	return (int)size;
}

/*
 * Memory helpers that murm386 keeps in SRAM (dos_api.c) so hot native paths
 * never execute from XIP flash.  On the host they are ordinary functions with
 * the same contracts: dos_api_memcpy copies forward, so an overlapping copy to
 * a higher address replicates the source like REP MOVSB, as the RP2350
 * byte/word loop does for such ranges.
 */
void *dos_api_memcpy(void *dst, const void *src, size_t len)
{
	uint8_t *d = (uint8_t *)dst;
	const uint8_t *s = (const uint8_t *)src;
	if (d > s && d < s + len) {
		while (len--)
			*d++ = *s++;
		return dst;
	}
	return memmove(dst, src, len);
}

const void *dos_api_memchr(const void *src, int value, size_t len)
{
	return memchr(src, value, len);
}

void *nf_memset(void *ptr, int value, size_t len)
{
	return memset(ptr, value, len);
}
