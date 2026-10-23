cmake_minimum_required(VERSION 3.16)
project(orange386 VERSION 1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
#set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")
set(CMAKE_C_FLAGS_RELEASE "-O0 -g")

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBDRM REQUIRED IMPORTED_TARGET libdrm)

add_executable(orange386
    src/main.c
    src/platform_linux.c
    src/ff_posix.c
    src/video_drm.c
    src/vga_hw_linux.c
    src/pc.c
    src/video_profile.c
    src/i386.c
    src/fpu.c
    src/bios/dispatch.c

    # Devices
    src/vga.c        # VGA emulation
    src/disk.c       # INT 13h disk handler (from pico-286)
    src/ide.c
    src/fdd.c

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
# vga.c keeps murm386's software framebuffer renderer for non-scanout hosts;
# like the RP2350 build, this port scans out through vga_hw_linux.c instead.
set_source_files_properties(src/vga.c PROPERTIES COMPILE_OPTIONS -Wno-unused-function)

find_package(Threads REQUIRED)
target_link_libraries(orange386 PRIVATE PkgConfig::LIBDRM Threads::Threads m)
target_compile_options(orange386 PRIVATE
    -Wall
    -Wextra
    -Wno-unused-variable
    -Wno-unused-parameter
    -Wno-old-style-declaration
)
target_include_directories(orange386 PUBLIC src)
target_compile_definitions(orange386 PRIVATE
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE=1
    ORANGE386=1
    NO_PAGING=1
    VGA256=1
    I386_MODE=1
    EMU_CPU_GEN=3
    # Data directory (BIOS images, config.ini, disk images), relative to
    # the working directory; same names as murm386's SD-card layout.
    SD_DATA_DIR="386"
    SD_DATA_DIR_SLASH="386/"
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
