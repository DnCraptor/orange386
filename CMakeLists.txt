cmake_minimum_required(VERSION 3.16)
project(kms_mattress VERSION 1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBDRM REQUIRED IMPORTED_TARGET libdrm)

add_executable(kms-mattress src/main.c)
target_link_libraries(kms-mattress PRIVATE PkgConfig::LIBDRM)
target_compile_options(kms-mattress PRIVATE -Wall -Wextra -Wpedantic)
install(TARGETS kms-mattress RUNTIME DESTINATION bin)
