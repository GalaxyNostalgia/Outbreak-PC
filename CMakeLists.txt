# Biohazard Outbreak PC
#
# Windows (MSYS2 "MINGW32" shell, or cross from Linux with cmake/mingw-i686.cmake):
#   cmake -S . -B build -DOUTBREAK_GAME=f1
#   cmake --build build
#   build\outbreak-f1.exe --test "Bio Hazard Outbreak - File 1.iso"
#
# Needs the decomp (git submodule in decomp/) configured with your own disc first:
#   cd decomp && python configure.py f1
cmake_minimum_required(VERSION 3.19)
project(outbreak_pc C ASM)

set(OUTBREAK_GAME "f1" CACHE STRING "f1 (Biohazard Outbreak) or f2 (File 2)")
set(OUTBREAK_DECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/decomp" CACHE PATH "the Outbreak-decomp checkout")
option(OUTBREAK_PLATFORM_ONLY "build only the platform layer (CI without game files)" OFF)

if(OUTBREAK_GAME STREQUAL "f1")
  set(GAME_ELF "SLPM_654.28")
elseif(OUTBREAK_GAME STREQUAL "f2")
  set(GAME_ELF "SLPM_656.92")
else()
  message(FATAL_ERROR "OUTBREAK_GAME must be f1 or f2")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "Build 32-bit: game structures and network packets use the PS2's 4-byte pointers. "
                      "Windows: MSYS2 MINGW32 shell or cmake/mingw-i686.cmake. Linux: -DCMAKE_C_FLAGS=-m32 -DCMAKE_ASM_FLAGS=-m32")
endif()

if(WIN32)
  # PS2 (GCC-style) bitfield layout, not Microsoft's
  add_compile_options(-mno-ms-bitfields)
endif()

set(PLATFORM_SOURCES src/iso9660.c src/sce_io.c src/libc_extra.c src/log.c src/gs.c src/display.c src/sce_graph.c src/cri_adx.c src/sce_cdvd.c src/overlay.c src/tsnddrv.c
    src/ps2ram.c src/hw.c src/kernel.c src/sce_sif.c src/sound.c src/asm.c src/dma.c src/pad.c src/mc2.c)
add_library(platform STATIC ${PLATFORM_SOURCES})
target_include_directories(platform PUBLIC src PRIVATE ${OUTBREAK_DECOMP_DIR}/include)
target_compile_options(platform PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_compile_definitions(platform PRIVATE _FILE_OFFSET_BITS=64 PLATFORM_PC)

# SDL2 for the window (optional: without it the program runs headless and can dump frames)
find_package(SDL2 CONFIG QUIET)
if(SDL2_FOUND)
  target_compile_definitions(platform PRIVATE HAVE_SDL2)
  if(TARGET SDL2::SDL2-static AND WIN32)
    target_link_libraries(platform PUBLIC SDL2::SDL2-static)
  else()
    target_link_libraries(platform PUBLIC SDL2::SDL2)
  endif()
  message(STATUS "SDL2 found: window enabled")
else()
  message(STATUS "SDL2 not found: headless build (--dump-frames writes the picture to files)")
endif()
if(OUTBREAK_PLATFORM_ONLY)
  return()
endif()

set(D ${OUTBREAK_DECOMP_DIR})
if(NOT EXISTS ${D}/asm/${GAME_ELF}/main)
  message(FATAL_ERROR "${D}/asm/${GAME_ELF} is missing: run `python configure.py ${OUTBREAK_GAME}` in ${D} "
                      "with your own disc image first (see README).")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
file(GLOB_RECURSE GAME_SOURCES CONFIGURE_DEPENDS ${D}/src/${GAME_ELF}/*.c)
file(GLOB_RECURSE PC_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)
set(GEN ${CMAKE_CURRENT_BINARY_DIR}/gen)
if(WIN32)
  set(UNDERSCORE --underscore)       # 32-bit Windows C symbols start with _
endif()

# overlays the decompiled C lives in, plus the main executable
set(TARGETS main)
foreach(f ${GAME_SOURCES})
  file(RELATIVE_PATH rel ${D}/src/${GAME_ELF} ${f})
  string(REGEX MATCH "^[^/]+" t ${rel})
  list(APPEND TARGETS ${t})
endforeach()
# plus the overlays config/<game>.json always links (e.g. the resident submain overlay)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/config/${OUTBREAK_GAME}.json GAME_CFG)
string(JSON n_ovl ERROR_VARIABLE no_ovl LENGTH "${GAME_CFG}" overlays)
if(NOT no_ovl AND n_ovl GREATER 0)
  math(EXPR last "${n_ovl} - 1")
  foreach(i RANGE ${last})
    string(JSON o GET "${GAME_CFG}" overlays ${i})
    list(APPEND TARGETS ${o})
  endforeach()
endif()
list(REMOVE_DUPLICATES TARGETS)
set(GEN_OUT ${GEN}/stubs.c ${GEN}/overlays.c ${GEN}/aliases.rsp)
foreach(t ${TARGETS})
  list(APPEND GEN_OUT ${GEN}/data_${t}.s)
  if(NOT t STREQUAL "main")
    list(APPEND GEN_OUT ${GEN}/names_${t}.h)
  endif()
endforeach()
add_custom_command(
  OUTPUT ${GEN_OUT}
  COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_data.py
          --decomp ${D} --game ${OUTBREAK_GAME} --out ${GEN} ${UNDERSCORE}
  DEPENDS ${GAME_SOURCES} ${PC_SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_data.py
          ${CMAKE_CURRENT_SOURCE_DIR}/config/${OUTBREAK_GAME}.json
  COMMENT "Converting the game's data from your ${GAME_ELF} (tools/gen_data.py)")

# decompiled game code: the same C files the decomp matches byte-for-byte on PS2
add_library(game OBJECT ${GAME_SOURCES})
set_target_properties(game PROPERTIES POSITION_INDEPENDENT_CODE OFF)
target_include_directories(game PRIVATE ${D}/include)
string(TOUPPER "VERSION_${OUTBREAK_GAME}" VERSION_DEF)
# NON_MATCHING: functions decompiled correctly but not yet byte-identical on PS2 are still built here
target_compile_definitions(game PRIVATE PLATFORM_PC NON_MATCHING ${VERSION_DEF})
target_compile_options(game PRIVATE -fno-builtin -fno-strict-aliasing -fwrapv -w)
foreach(f ${GAME_SOURCES})
  file(RELATIVE_PATH rel ${D}/src/${GAME_ELF} ${f})
  string(REGEX MATCH "^[^/]+" t ${rel})
  if(NOT t STREQUAL "main")
    set_source_files_properties(${f} PROPERTIES COMPILE_OPTIONS "-include;${GEN}/names_${t}.h"
                                OBJECT_DEPENDS ${GEN}/names_${t}.h)
  endif()
endforeach()

set(GAME_DATA)
foreach(t ${TARGETS})
  list(APPEND GAME_DATA ${GEN}/data_${t}.s)
endforeach()
add_executable(outbreak-${OUTBREAK_GAME} src/main.c ${GEN}/stubs.c ${GEN}/overlays.c ${GAME_DATA} $<TARGET_OBJECTS:game>)
target_include_directories(outbreak-${OUTBREAK_GAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
# names the decompiled C uses for overlay functions (tools/gen_data.py)
target_link_options(outbreak-${OUTBREAK_GAME} PRIVATE @${GEN}/aliases.rsp)
set_property(TARGET outbreak-${OUTBREAK_GAME} APPEND PROPERTY LINK_DEPENDS ${GEN}/aliases.rsp)
target_link_libraries(outbreak-${OUTBREAK_GAME} PRIVATE platform)
if(UNIX)
  target_link_libraries(outbreak-${OUTBREAK_GAME} PRIVATE m)   # atan2, sqrt
endif()
target_compile_definitions(outbreak-${OUTBREAK_GAME} PRIVATE GAME_ELF="${GAME_ELF}")
if(WIN32)
  target_link_options(outbreak-${OUTBREAK_GAME} PRIVATE -static)   # one .exe, no MinGW DLLs to ship
  # at its usual low base, never relocated: the game keeps flags in the top bits of addresses
  target_link_options(outbreak-${OUTBREAK_GAME} PRIVATE -Wl,--disable-dynamicbase -Wl,--image-base=0x400000)
else()
  target_link_options(outbreak-${OUTBREAK_GAME} PRIVATE -no-pie)
endif()
