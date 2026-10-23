#ifndef HARDWARE_TIMER_H
#define HARDWARE_TIMER_H

#include <stdint.h>
#include <time.h>

static inline uint64_t time_us_64(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (uint64_t)ts.tv_sec * 1000000ULL +
           (uint64_t)ts.tv_nsec / 1000ULL;
}

static inline uint32_t time_us_32(void)
{
    return (uint32_t)time_us_64();
}

#endif
