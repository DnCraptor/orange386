#include "pc.h"
#include "mem.h"
#include "platform_linux.h"
#include "ff.h"
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
 * Load ROM file from the data directory into guest memory (via FatFs, so
 * names are case-insensitive and "/386/..." paths from config.ini resolve
 * below the data root, as on the SD card).
 * backward != 0: the image ends at addr (system BIOS below 1 MiB).
 * Returns the image size, or -1.
 */
int load_rom(void *phys_mem, const char *file, uword addr, int backward)
{
	FIL fp;
	FRESULT res;
	UINT bytes_read;

	char path[FF_LFN_BUF + 1 + sizeof(SD_DATA_DIR_SLASH)];
	if (strchr(file, '/') || strchr(file, '\\') || (file[0] && file[1] == ':'))
		snprintf(path, sizeof(path), "%s", file);
	else
		snprintf(path, sizeof(path), SD_DATA_DIR_SLASH "%s", file);

	res = f_open(&fp, path, FA_READ);
	if (res != FR_OK) {
		printf("Failed to open ROM: %s (error %d)\n", path, res);
		return -1;
	}

	FSIZE_t size = f_size(&fp);
	if (size == 0 || size > 0x100000 || (backward && size > addr)) {
		printf("ERROR: invalid ROM size: %s (%lu bytes)\n", path,
		       (unsigned long)size);
		f_close(&fp);
		return -1;
	}

	uint8_t *dest = backward ? (uint8_t *)phys_mem + addr - size
				 : (uint8_t *)phys_mem + addr;
	res = f_read(&fp, dest, (UINT)size, &bytes_read);
	f_close(&fp);
	if (res != FR_OK || bytes_read != size) {
		printf("ERROR: Failed to read ROM: %s (error %d, read %u of %lu)\n",
		       path, res, bytes_read, (unsigned long)size);
		return -1;
	}
	printf("Loaded ROM: %s (%lu bytes) at 0x%05lx\n", path, (unsigned long)size,
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

/*
 * Pico SDK repeating timers (pico/time.h) on host threads.
 */
#include <pico/time.h>

static uint64_t mono_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void sleep_until_us(uint64_t t)
{
	struct timespec ts = {
		.tv_sec = (time_t)(t / 1000000u),
		.tv_nsec = (long)(t % 1000000u) * 1000L,
	};
	while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
		;
}

static void *repeating_timer_main(void *arg)
{
	repeating_timer_t *rt = arg;
	uint64_t period = (uint64_t)(rt->delay_us < 0 ? -rt->delay_us : rt->delay_us);
	uint64_t next = mono_us() + period;
	while (rt->running) {
		sleep_until_us(next);
		if (!rt->running)
			break;
		uint64_t start = mono_us();
		if (!rt->callback(rt))
			break;
		/* delay_us > 0: end-to-start; delay_us < 0: start-to-start */
		next = rt->delay_us > 0 ? mono_us() + period : start + period;
	}
	rt->running = false;
	return NULL;
}

bool add_repeating_timer_us(int64_t delay_us, repeating_timer_callback_t callback,
			    void *user_data, repeating_timer_t *out)
{
	if (!out || !callback || delay_us == 0)
		return false;
	out->delay_us = delay_us;
	out->callback = callback;
	out->user_data = user_data;
	out->running = true;
	if (pthread_create(&out->thread, NULL, repeating_timer_main, out) != 0) {
		out->running = false;
		return false;
	}
	return true;
}

bool cancel_repeating_timer(repeating_timer_t *timer)
{
	if (!timer || !timer->thread)
		return false;
	bool was_running = timer->running;
	timer->running = false;
	pthread_join(timer->thread, NULL);
	timer->thread = 0;
	return was_running;
}
