#ifndef ORANGE386_PICO_TIME_H
#define ORANGE386_PICO_TIME_H

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>
#include <hardware/timer.h>

/*
 * Linux host replacement for the Pico SDK repeating timers (<pico/time.h>).
 *
 * On RP2350 a repeating timer is an alarm-pool interrupt on the core that
 * created it.  Here each timer runs its callback on its own thread, paced by
 * CLOCK_MONOTONIC.  As in the SDK, a positive delay_us is measured from the
 * end of one callback to the start of the next, a negative one from start to
 * start; the callback returns false to stop the timer.
 */

typedef struct repeating_timer repeating_timer_t;
typedef bool (*repeating_timer_callback_t)(repeating_timer_t *rt);

struct repeating_timer {
	int64_t delay_us;
	void *user_data;
	repeating_timer_callback_t callback;
	pthread_t thread;
	volatile bool running;
};

bool add_repeating_timer_us(int64_t delay_us, repeating_timer_callback_t callback,
			    void *user_data, repeating_timer_t *out);
bool cancel_repeating_timer(repeating_timer_t *timer);

/* Busy-free sleeps of the calling thread (SDK: sleep_us / sleep_ms). */
static inline void sleep_us(uint64_t us)
{
	struct timespec ts = {
		.tv_sec = (time_t)(us / 1000000u),
		.tv_nsec = (long)(us % 1000000u) * 1000L,
	};
	while (nanosleep(&ts, &ts) != 0)
		;
}

static inline void sleep_ms(uint32_t ms)
{
	sleep_us((uint64_t)ms * 1000u);
}

#endif /* ORANGE386_PICO_TIME_H */
