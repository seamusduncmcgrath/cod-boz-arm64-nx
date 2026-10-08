cmake_minimum_required(VERSION 3.21)

# Nintendo Switch port of Call of Duty: Black Ops Zombies: a loader for the game's
# 64-bit (AArch64) Marmalade S3E image, built with devkitA64 and libnx.
project(boz-nx
    VERSION 0.1.0
    LANGUAGES C CXX
)

# NINTENDO_SWITCH is set by devkitPro's Switch.cmake toolchain file.
if(NOT NINTENDO_SWITCH)
    message(FATAL_ERROR
        "Not using the devkitPro Switch toolchain. Configure with a preset: cmake --preset debug")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# SDL2, SDL2_mixer and Mesa come from devkitPro portlibs:
#   pacman -S switch-sdl2 switch-sdl2_mixer switch-mesa switch-libdrm_nouveau
find_package(PkgConfig REQUIRED)
pkg_check_modules(BOZ_DEPS REQUIRED IMPORTED_TARGET SDL2_mixer sdl2 egl)

add_executable(boz
    src/codboz_hud_handlers.c
    src/device_id.c
    src/mdns_wire.c
    src/s3e_audio.c
    src/s3e_audio_unit.c
    src/s3e_config.c
    src/s3e_egl.c
    src/s3e_file.c
    src/s3e_gl.c
    src/s3e_host.c
    src/s3e_image.c
    src/s3e_input.c
    src/s3e_memory.c
    src/s3e_runtime.c
    src/s3e_secure_storage.c
    src/s3e_socket.c
    src/s3e_timer.c
    src/s3e_zeroconf.c
    src/zeroconf_platform_posix.c
    src/switch/switch_code_memory.c
    src/switch/switch_dl.c
    src/switch/switch_main.c
    third_party/lzma/LzmaDec.c
)

target_include_directories(boz PRIVATE
    src/switch/compat
    src/switch
    include
    third_party/lzma
)
target_compile_definitions(boz PRIVATE _GNU_SOURCE BOZ_VERSION="${PROJECT_VERSION}")
# The two disabled warnings only fire on code that is correct as written: an overflow guard that
# is vacuous with a 64-bit size_t, and snprintf calls that truncate on purpose.
target_compile_options(boz PRIVATE -Wall -Wextra -Wno-type-limits -Wno-format-truncation)
target_link_libraries(boz PRIVATE PkgConfig::BOZ_DEPS)

# Mesa is C++, so the final link has to go through g++.
set_target_properties(boz PROPERTIES LINKER_LANGUAGE CXX)

# Title metadata shown in hbmenu.
nx_generate_nacp(
    OUTPUT boz.nacp
    NAME "Black Ops Zombies"
    AUTHOR "Seamus McGrath"
    VERSION ${PROJECT_VERSION}
)

# Converts boz.elf into boz.nro, the file you copy to sdmc:/switch/boz/.
nx_create_nro(boz
    NACP boz.nacp
)
