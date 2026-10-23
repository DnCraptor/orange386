cmake_minimum_required(VERSION 3.16)
project(orange386 VERSION 1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
#set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")
set(CMAKE_C_FLAGS_RELEASE "-O3 -g")

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBDRM REQUIRED IMPORTED_TARGET libdrm)
pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)

add_executable(orange386
    src/main.c
    src/platform_linux.c
    src/ff_posix.c
    src/video_drm.c
    src/input_linux.c
    src/audio_linux.c
    src/config_save.c
    src/vga_hw_linux.c
    src/vga_osd.c    # OSD text plane (Disk Manager / Settings)
    src/font8x16.c
    src/diskui.c     # Disk Manager (Win+F12)
    src/settingsui.c # Settings (Win+F11)
    src/pc.c
    src/video_profile.c
    src/i386.c
    src/fpu.c
    # Native BIOS (bios=native), ported from murm386's RP2350 SeaBIOS port
    src/bios/dispatch.c
    src/bios/bios_00h.c
    src/bios/bios_05h.c
    src/bios/bios_08h.c
    src/bios/bios_09h.c
    src/bios/bios_10h.c
    src/bios/bios_11h.c
    src/bios/bios_12h.c
    src/bios/bios_13h.c
    src/bios/bios_14h.c
    src/bios/bios_15h.c
    src/bios/bios_16h.c
    src/bios/bios_17h.c
    src/bios/bios_18h.c
    src/bios/bios_19h.c
    src/bios/bios_1Ah.c
    src/bios/bios_33h.c
    src/bios/bios_FFh.c
    src/bios/bios_intcall.c
    src/bios/bios_printf.c
    src/font8x8.c
    src/font8x14.c

    # Devices
    src/vga.c        # VGA emulation
    src/disk.c       # INT 13h disk handler (from pico-286)
    src/ide.c
    src/fdd.c

    # Sound devices
    src/adlib.c      # Adlib/OPL2
    src/sb16.c       # Sound Blaster 16
    src/sn76489.c    # Tandy 3-Voice Sound (SN76489)
    src/dss.c        # Disney Sound Source
    src/csm.c        # Covox Sound Master AYDMA/DAC
    src/csm_psg.c    # Covox Sound Master PSG (AY8930)
    # FM OPL synthesis engine
    src/emu8950/emu8950.c
    src/emu8950/emuadpcm.c
    src/emu8950/slot_render.cpp
    src/gameport.c   # Analog game port

    # Peripheral chips
    src/i8042.c      # Keyboard controller
    src/i8254.c      # Timer
    src/i8257.c      # DMA
    src/i8259.c      # PIC
    src/pci.c        # PCI bus
    src/misc.c       # Serial, RTC, CMOS
    src/pcspk.c      # PC Speaker (port 61h)

    # Configuration
    src/ini.c
)
find_package(Threads REQUIRED)
target_link_libraries(orange386 PRIVATE PkgConfig::LIBDRM PkgConfig::ALSA Threads::Threads m)
target_compile_options(orange386 PRIVATE
    -Wall
    -Wextra
    -Wno-unused-variable
    -Wno-unused-parameter
    $<$<COMPILE_LANGUAGE:C>:-Wno-old-style-declaration>
)
target_include_directories(orange386 PUBLIC src)
target_compile_definitions(orange386 PRIVATE
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE=1
    ORANGE386=1
    # Native FreeDOS kernel (src/fdos) is not built yet: native BIOS boots
    # guest media only.
    NATIVE_DOS=0
    NO_PAGING=1
    VGA256=1
    I386_MODE=1
    EMU_CPU_GEN=3
    # Data directory (BIOS images, config.ini, disk images), relative to
    # the working directory; same names as murm386's SD-card layout.
    SD_DATA_DIR="386"
    SD_DATA_DIR_SLASH="386/"

    SOUND_FREQUENCY=44100

    # OPL2 (same emu8950 configuration as murm386; without PICO_ON_DEVICE
    # the slot renderer uses its portable path instead of the SIO
    # interpolators)
    USE_EMU8950_OPL
    EMU8950_SLOT_RENDER=1
    EMU8950_NO_RATECONV=1
    EMU8950_NO_WAVE_TABLE_MAP=1
    EMU8950_NO_TLL=1
    EMU8950_NO_FLOAT=1
    EMU8950_NO_TIMER=1
    EMU8950_NO_TEST_FLAG=1
    EMU8950_SIMPLER_NOISE=1
    EMU8950_SHORT_NOISE_UPDATE_CHECK=1
    EMU8950_LINEAR_SKIP=1
    EMU8950_LINEAR_END_OF_NOTE_OPTIMIZATION
    EMU8950_NO_PERCUSSION_MODE=1
    EMU8950_LINEAR=1
)
install(TARGETS orange386 RUNTIME DESTINATION bin)

# ROM images and configuration are project-owned resources, never fetched at build time.
set(ORANGE386_RESOURCES
    "${CMAKE_SOURCE_DIR}/386/bios.bin"
    "${CMAKE_SOURCE_DIR}/386/vgabios.bin"
    "${CMAKE_SOURCE_DIR}/386/config.ini"
    "${CMAKE_SOURCE_DIR}/386/fdd0.img"
    "${CMAKE_SOURCE_DIR}/386/fdd0dr81.img"
    "${CMAKE_SOURCE_DIR}/386/fdd0free.img"
)
foreach(resource IN LISTS ORANGE386_RESOURCES)
    if(NOT EXISTS "${resource}")
        message(FATAL_ERROR "Missing bundled orange386 resource: ${resource}")
    endif()
endforeach()
add_custom_command(TARGET orange386 POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:orange386>/386"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        ${ORANGE386_RESOURCES} "$<TARGET_FILE_DIR:orange386>/386"
    COMMENT "Copy bundled BIOS ROMs and config.ini into the executable directory"
    VERBATIM
)
