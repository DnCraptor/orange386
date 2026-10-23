#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

/*
 * Board configuration for the Linux host (Orange Pi Zero 2W).
 *
 * murm386's board_config.h describes RP2350 pins, clocks and the SD-card
 * layout.  Only the layout is meaningful here.  The RP2350 clock settings
 * (cpu_freq, psram_freq, flash_freq) are still carried by config_save.c so
 * a shared config.ini keeps them, but the host does not use them.
 */

#define BOARD_VARIANT_STR "OPIZ2W"

#ifndef CPU_CLOCK_MHZ
#define CPU_CLOCK_MHZ 0
#endif
#ifndef PSRAM_MAX_FREQ_MHZ
#define PSRAM_MAX_FREQ_MHZ 0
#endif
#ifndef FLASH_MAX_FREQ_MHZ
#define FLASH_MAX_FREQ_MHZ 0
#endif

#ifndef SD_DATA_DIR
#define SD_DATA_DIR "386"
#endif
#ifndef SD_DATA_DIR_SLASH
#define SD_DATA_DIR_SLASH SD_DATA_DIR "/"
#endif

/* Persistent settings live next to the BIOS and disk images. */
#define CONFIG_ROOT_DIR  SD_DATA_DIR
#define CONFIG_CPU_DIR   SD_DATA_DIR
#define CONFIG_BOARD_DIR SD_DATA_DIR
#define CONFIG_FILE_PATH SD_DATA_DIR_SLASH "config.ini"

#endif // BOARD_CONFIG_H
