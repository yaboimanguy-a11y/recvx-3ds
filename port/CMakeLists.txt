# Resident Evil: Code Veronica X, native port: New Nintendo 3DS.
#
#   cmake -S port -B build/port-3ds -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_TOOLCHAIN_FILE=$PWD/port/cmake/3ds-clang.cmake
#   ninja -C build/port-3ds recvx_sdcard     (-> build/port-3ds/sdcard/3ds/recvx/)
#
# The game's C (src/) is first copied through port/tools/hostify.py into
# <build>/hostsrc with the source-level fixes a 32-bit host compiler needs;
# the originals keep building the PS2 executable.
cmake_minimum_required(VERSION 3.20)
# optimised by default: a build without this runs far too slowly on the 3DS
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "build type" FORCE)
endif()
project(recvx_port C ASM)

# the original 3DS (RECVX_O3DS=ON, its own build folder): less memory, no 3D
# slider, no updater, frame skip on by default; a separate app, recvx_o3ds.3dsx
option(RECVX_O3DS "build for the original 3DS / 2DS" OFF)
if(RECVX_O3DS)
  add_compile_definitions(RECVX_O3DS=1)
endif()

if(NOT RECVX_3DS)
  message(FATAL_ERROR "configure with -DCMAKE_TOOLCHAIN_FILE=port/cmake/3ds-clang.cmake")
endif()

get_filename_component(RECVX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(PORT_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(HOSTSRC "${CMAKE_BINARY_DIR}/hostsrc")
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# hostify at configure time (and again whenever a game source changes: the
# glob below re-runs configure through CONFIGURE_DEPENDS)
file(GLOB_RECURSE _orig_srcs CONFIGURE_DEPENDS
  "${RECVX_ROOT}/src/*.c" "${RECVX_ROOT}/include/ps2/*.h" "${RECVX_ROOT}/include/cri/*.h"
  "${PORT_DIR}/tools/hostify.py")
execute_process(COMMAND ${Python3_EXECUTABLE} "${PORT_DIR}/tools/hostify.py" "${HOSTSRC}"
                RESULT_VARIABLE _hr)
if(_hr)
  message(FATAL_ERROR "hostify failed")
endif()

# ---- the game (EE side; IOP modules and the JPN-only files left out) -------
file(GLOB_RECURSE GAME_SRCS "${HOSTSRC}/src/*.c")
list(FILTER GAME_SRCS EXCLUDE REGEX "/src/cri/ps2/|/src/JPN/|/src/users/oshimi/adx/")
# the ADX renderer thunks: port/ps2/adx_out.c plays the PCM instead
list(FILTER GAME_SRCS EXCLUDE REGEX "/libadxe/adx_rnap\\.c$")
foreach(_iop atick.c tq.c tsnddrv.c putsts.c sdd_com.c)
  list(FILTER GAME_SRCS EXCLUDE REGEX "/src/${_iop}$")
endforeach()

set(KATANA "${RECVX_ROOT}/include/recvx-decomp-katana/KATANA/Include")
set(GAME_INCLUDES
  "${PORT_DIR}/shim" "${PORT_DIR}/host" "${PORT_DIR}/include"
  "${HOSTSRC}/include/ps2/veronica/prog" "${HOSTSRC}/include" "${RECVX_ROOT}/include"
  "${RECVX_ROOT}/include/recvx-decomp-cri/cri/mwlib/include"
  "${RECVX_ROOT}/include/recvx-decomp-cri/cri/mwlib/ee/include"
  "${HOSTSRC}/include/cri/mwlib/ee/lib/libadxe")

# EE enums are int-sized: everything that includes game headers is built
# with -fno-short-enums; 3DS platform code (libctru, short enums) talks to it
# through plain-C interfaces only.
# -fwritable-strings: the PS2 had no write protection and the game edits
# some string literals in place (gdFsInit upper-cases its file table)
set(GAME_FLAGS -fno-short-enums -fsigned-char -DRECVX_PORT -std=gnu99 -fno-strict-aliasing -fwrapv -fwritable-strings
  -include "${PORT_DIR}/include/port_pre.h"
  -Wno-implicit-function-declaration -Wno-implicit-int -Wno-int-conversion
  -Wno-incompatible-pointer-types -Wno-return-type -Wno-incompatible-function-pointer-types
  -Wno-everything)

add_library(recvx_game STATIC ${GAME_SRCS}
  "${PORT_DIR}/host/na_math_port.c"
  "${PORT_DIR}/ps2/ps2_stubs.c"
  "${PORT_DIR}/ps2/draw_stubs.c"
  "${PORT_DIR}/ps2/kernel.c"
  "${PORT_DIR}/ps2/io.c"
  "${PORT_DIR}/ps2/disc.c"
  "${PORT_DIR}/ps2/gs.c"
  "${PORT_DIR}/ps2/trace.c"
  "${PORT_DIR}/ps2/diag.c"
  "${PORT_DIR}/ps2/movie_stub.c"
  "${PORT_DIR}/ps2/vu_strip.c"
  "${PORT_DIR}/ps2/radar.c"
  "${PORT_DIR}/ps2/adx_out.c"
  "${PORT_DIR}/ps2/snd_synth.c")
target_include_directories(recvx_game PRIVATE "${PORT_DIR}/ps2")
target_include_directories(recvx_game PRIVATE ${GAME_INCLUDES})
target_include_directories(recvx_game SYSTEM PRIVATE ${RECVX_3DS_SYSTEM_INCLUDES})
target_compile_options(recvx_game PRIVATE ${GAME_FLAGS})
# the Dreamcast SDK headers (ninja.h, sg_*.h) after the C library's own,
# which they would otherwise shadow (stddef.h)
target_compile_options(recvx_game PRIVATE "-idirafter" "${KATANA}")
# the port's drawing code and the game's matrix / vector routines
# (port/host/na_math_port.c, VU0 work on the PS2, which is not IEEE either):
# floating point the compiler may fuse and reorder
set_source_files_properties("${PORT_DIR}/ps2/vu_strip.c" "${PORT_DIR}/ps2/gs.c" "${PORT_DIR}/ps2/radar.c"
  "${PORT_DIR}/host/na_math_port.c"
  PROPERTIES COMPILE_OPTIONS "-ffast-math")
# debug: record function entries so a hang can be located (port/ps2/trace.c)
option(RECVX_TRACE "instrument game functions for the hang heartbeat (slow)" OFF)
if(RECVX_TRACE)
  target_compile_options(recvx_game PRIVATE -finstrument-functions)
endif()

# the PS2 game's main() becomes recvx_ps2_main(); the 3DS app has its own
set_source_files_properties("${HOSTSRC}/src/ps2/veronica/prog/njloop.c"
  PROPERTIES COMPILE_DEFINITIONS "main=recvx_ps2_main")

# ---- the 3DS app -------------------------------------------------------------
function(recvx_3dsx target title)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND "${RECVX_SMDHTOOL}" --create "${title}" "Native port - test build"
            "recvx-port" "${DEVKITPRO}/libctru/default_icon.png" "${CMAKE_BINARY_DIR}/${target}.smdh"
    COMMAND "${RECVX_3DSXTOOL}" "$<TARGET_FILE:${target}>" "${CMAKE_BINARY_DIR}/${target}.3dsx"
            "--smdh=${CMAKE_BINARY_DIR}/${target}.smdh"
    COMMENT "3dsxtool: ${target}.3dsx" VERBATIM)
endfunction()

# vertex shader (picasso) as a C array
set(_shbin "${CMAKE_BINARY_DIR}/r2d.shbin")
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/r2d_shbin.c"
  COMMAND "${DEVKITPRO}/tools/bin/picasso" -o "${_shbin}" "${PORT_DIR}/render/3ds/r2d.v.pica"
  COMMAND ${Python3_EXECUTABLE} "${PORT_DIR}/cmake/bin2c.py" "${_shbin}" "${CMAKE_BINARY_DIR}/r2d_shbin.c" r2d_shbin
  DEPENDS "${PORT_DIR}/render/3ds/r2d.v.pica" "${PORT_DIR}/cmake/bin2c.py"
  COMMENT "picasso: r2d.v.pica")

add_executable(recvx
  "${PORT_DIR}/platform/3ds/main_3ds.c"
  "${PORT_DIR}/platform/3ds/sys_3ds.c"
  "${PORT_DIR}/platform/3ds/audio_3ds.c"
  "${PORT_DIR}/platform/3ds/ui_3ds.c"
  "${PORT_DIR}/platform/3ds/update_3ds.c"
  "${PORT_DIR}/platform/3ds/inflate.c"
  "${PORT_DIR}/render/3ds/rend_c3d.c"
  "${CMAKE_BINARY_DIR}/r2d_shbin.c")
set_target_properties(recvx PROPERTIES SUFFIX ".elf")
target_include_directories(recvx PRIVATE "${PORT_DIR}/platform/3ds" "${PORT_DIR}/ps2")
target_include_directories(recvx SYSTEM PRIVATE ${RECVX_3DS_SYSTEM_INCLUDES}
  "${DEVKITPRO}/libctru/include")
target_compile_options(recvx PRIVATE -Wall -Wno-unused-function)
set_source_files_properties("${PORT_DIR}/render/3ds/rend_c3d.c" PROPERTIES COMPILE_OPTIONS "-ffast-math")
# the in-app updater (update_3ds.c): this app's version, the GitHub
# repository whose releases it installs, and libcurl + mbedTLS from the
# devkitPro portlibs (dkp-pacman -S 3ds-curl)
set(RECVX_VERSION "0.1.0" CACHE STRING "version of the 3DS app (compared with the release tags, v0.1.0)")
set(RECVX_UPDATE_REPO "yaboimanguy-a11y/recvx-3ds" CACHE STRING "GitHub owner/repo the updater installs releases from")
set(_portlibs "${DEVKITPRO}/portlibs/3ds")
set(RECVX_UPDATER_LIBS "")
target_compile_definitions(recvx PRIVATE RECVX_VERSION="${RECVX_VERSION}" RECVX_UPDATE_REPO="${RECVX_UPDATE_REPO}")
if(RECVX_O3DS)
  # no updater: its TLS memory does not fit, and it installs the New 3DS app
elseif(EXISTS "${_portlibs}/lib/libcurl.a" AND EXISTS "${_portlibs}/lib/libmbedtls.a")
  target_compile_definitions(recvx PRIVATE RECVX_HAVE_CURL=1)
  target_include_directories(recvx SYSTEM PRIVATE "${_portlibs}/include")
  set(RECVX_UPDATER_LIBS "${_portlibs}/lib/libcurl.a" "${_portlibs}/lib/libmbedtls.a"
    "${_portlibs}/lib/libmbedx509.a" "${_portlibs}/lib/libmbedcrypto.a" "${_portlibs}/lib/libz.a")
else()
  message(WARNING "3ds-curl not installed (dkp-pacman -S 3ds-curl): the app is built without its updater")
endif()
target_link_libraries(recvx PRIVATE recvx_game citro2d citro3d ${RECVX_UPDATER_LIBS} ctru m)
target_link_options(recvx PRIVATE "-Wl,-Map=${CMAKE_BINARY_DIR}/recvx.map")
if(RECVX_O3DS)
  recvx_3dsx(recvx "Resident Evil CVX (Old 3DS)")
  set(RECVX_SD_NAME recvx_o3ds.3dsx)
else()
  recvx_3dsx(recvx "Resident Evil CVX")
  set(RECVX_SD_NAME recvx.3dsx)
endif()

# ---- SD card folder ------------------------------------------------------
# the updater checks GitHub's certificate against cacert.pem (a CA bundle)
set(RECVX_CA_BUNDLE "/etc/ssl/cert.pem" CACHE FILEPATH "CA certificate bundle copied to the SD folder as cacert.pem")
set(SD "${CMAKE_BINARY_DIR}/sdcard/3ds/recvx")
add_custom_target(recvx_sdcard ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory "${SD}"
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_BINARY_DIR}/recvx.3dsx" "${SD}/${RECVX_SD_NAME}"
  COMMAND ${CMAKE_COMMAND} -E copy "${PORT_DIR}/platform/3ds/README.txt" "${SD}/README.txt"
  COMMAND ${CMAKE_COMMAND} -E copy "${PORT_DIR}/platform/3ds/README_O3DS.txt" "${SD}/README_O3DS.txt"
  COMMAND ${CMAKE_COMMAND} -E copy "${RECVX_CA_BUNDLE}" "${SD}/cacert.pem"
  DEPENDS recvx
  COMMENT "SD card folder: sdcard/3ds/recvx")
