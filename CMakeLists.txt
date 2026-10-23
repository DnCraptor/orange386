cmake_minimum_required(VERSION 3.16)
project(orange386 VERSION 1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBDRM REQUIRED IMPORTED_TARGET libdrm)

add_executable(orange386
    src/main.c
    src/i386.c
    src/fpu.c
    src/iomem.c
    src/iomem.c
)
target_link_libraries(orange386 PRIVATE PkgConfig::LIBDRM m)
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
    NO_PAGING=1
    I386_MODE=1
    I386_MODE=1
)
install(TARGETS orange386 RUNTIME DESTINATION bin)
