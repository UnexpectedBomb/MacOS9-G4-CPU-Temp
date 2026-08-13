cmake_minimum_required(VERSION 3.13)
project(CPUTempCSM C)

# Configure with the Retro68 PowerPC toolchain:
#   cmake -B build \
#     -DCMAKE_TOOLCHAIN_FILE=$HOME/Retro68-build/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake
#   cmake --build build

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED TRUE)
add_compile_options(-Wno-multichar -Wall)

# Build the module as a native PowerPC CFM code fragment (an import library),
# the format every shipping Control Strip module uses. The older approach wrapped
# the PEF in an 'sdev' Mixed-Mode resource; that loaded and ran, but the Control
# Strip would not let you drag a copy of the module out of the strip. A real
# 'cfrg' fragment (PEF in the data fork) is a first-class, draggable module.
add_library(CPUTempCSM SHARED cpu_temp_csm.c)
if(TARGET retrocrt)
    add_dependencies(CPUTempCSM retrocrt)
endif()

# Export the Control Strip entry point. We do NOT use the linker's -e to set the
# fragment entry (that drops the symbol from the export table); instead the
# symbol stays exported and patch-pef-main.py points the fragment `main` at it
# after MakePEF -- the same pattern the native 'ndrv' drivers use.
set_target_properties(CPUTempCSM PROPERTIES
    LINK_FLAGS "-Wl,-bE:${CMAKE_CURRENT_SOURCE_DIR}/cpu_temp_csm.exp")

# ControlStripLib: SB* helpers.  NameRegistryLib: Registry* (i2c discovery).
# QuickDraw / Menus / Notification / Memory / Gestalt resolve from InterfaceLib.
target_link_libraries(CPUTempCSM ControlStripLib NameRegistryLib)

# XCOFF shared lib -> PEF, then point the fragment `main` at ControlStripModule.
add_custom_command(OUTPUT CPUTempCSM.pef
    COMMAND ${MAKE_PEF} libCPUTempCSM.so -o CPUTempCSM.pef
    COMMAND /usr/bin/env python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/patch-pef-main.py
            CPUTempCSM.pef ControlStripModule
    DEPENDS CPUTempCSM ${CMAKE_CURRENT_SOURCE_DIR}/scripts/patch-pef-main.py)

if(REZ_INCLUDE_PATH)
    set(REZ_INCLUDE_FLAG -I${REZ_INCLUDE_PATH})
endif()

# Rez the resource fork ('cfrg' + Finder bundle + thermometer icon + 'vers'),
# put the PEF in the DATA FORK (--data), stamp file type 'sdev' / creator 'CPUt',
# then set the Finder flags (kHasBundle | kHasCustomIcon) via set-custom-icon.py.
# therm_icon.r is #included by cpu_temp_csm.r; Rez include deps are NOT tracked
# by CMake, so it MUST be in DEPENDS or icon edits silently won't re-Rez.
set(SET_CUSTOM_ICON ${CMAKE_CURRENT_SOURCE_DIR}/scripts/set-custom-icon.py)
add_custom_command(OUTPUT CPUTempCSM.bin
    COMMAND ${REZ} ${REZ_INCLUDE_FLAG}
                    ${CMAKE_CURRENT_SOURCE_DIR}/cpu_temp_csm.r
                    -o CPUTempCSM.bin
                    -t sdev -c CPUt
                    --data CPUTempCSM.pef
    COMMAND /usr/bin/env python3 ${SET_CUSTOM_ICON} CPUTempCSM.bin sdevCPUt
    DEPENDS CPUTempCSM.pef ${CMAKE_CURRENT_SOURCE_DIR}/cpu_temp_csm.r
            ${CMAKE_CURRENT_SOURCE_DIR}/therm_icon.r ${SET_CUSTOM_ICON})

add_custom_target(CPUTempCSM_sdev ALL DEPENDS CPUTempCSM.bin)
