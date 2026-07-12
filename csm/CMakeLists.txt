cmake_minimum_required(VERSION 3.13)
project(CPUTempCSM C)

# Configure with the Retro68 PowerPC toolchain:
#   cmake -B build \
#     -DCMAKE_TOOLCHAIN_FILE=$HOME/Retro68-build/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake
#   cmake --build build

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED TRUE)
add_compile_options(-Wno-multichar -Wall)

# A Control Strip module is a code resource, not an application.
add_executable(CPUTempCSM cpu_temp_csm.c)
if(TARGET retrocrt)
    add_dependencies(CPUTempCSM retrocrt)
endif()

set_target_properties(CPUTempCSM PROPERTIES
    COMPILE_OPTIONS -ffunction-sections
    LINK_FLAGS "-Wl,-eControlStripModule -Wl,-gc-sections")

# ControlStripLib: SB* helpers.  NameRegistryLib: Registry* (i2c discovery).
# NMInstall / QuickDraw / NumToString resolve from InterfaceLib (default).
target_link_libraries(CPUTempCSM ControlStripLib NameRegistryLib)

# XCOFF -> PEF (the native PowerPC code fragment the 'rdes' wraps).
add_custom_command(OUTPUT CPUTempCSM.pef
    COMMAND ${MAKE_PEF} CPUTempCSM -o CPUTempCSM.pef
    DEPENDS CPUTempCSM)

if(REZ_INCLUDE_PATH)
    set(REZ_INCLUDE_FLAG -I${REZ_INCLUDE_PATH})
endif()

# Rez the PEF into an 'sdev' resource and stamp file type 'sdev' (REQUIRED for
# the Control Strip to load it) + creator 'CPUt'.  Emit only the .bin we deploy
# (see UT-launcher note: a colliding --cc name corrupts the XCOFF).
# Rez the 'sdev' + thermometer icon family, then flip on kHasCustomIcon so the
# Finder shows the icon (Rez leaves fdFlags=0).  therm_icon.r is #included by
# cpu_temp_csm.r; Rez include deps are NOT auto-tracked by CMake, so it MUST be
# in DEPENDS or icon edits silently won't re-Rez.
set(SET_CUSTOM_ICON ${CMAKE_CURRENT_SOURCE_DIR}/scripts/set-custom-icon.py)
add_custom_command(OUTPUT CPUTempCSM.bin
    COMMAND ${REZ} ${REZ_INCLUDE_FLAG}
                    ${CMAKE_CURRENT_SOURCE_DIR}/cpu_temp_csm.r
                    -o CPUTempCSM.bin
                    -t sdev -c CPUt
    COMMAND /usr/bin/env python3 ${SET_CUSTOM_ICON} CPUTempCSM.bin sdevCPUt
    DEPENDS CPUTempCSM.pef ${CMAKE_CURRENT_SOURCE_DIR}/cpu_temp_csm.r
            ${CMAKE_CURRENT_SOURCE_DIR}/therm_icon.r ${SET_CUSTOM_ICON})

add_custom_target(CPUTempCSM_sdev ALL DEPENDS CPUTempCSM.bin)
