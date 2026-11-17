# =============================================================================
# cmake/arm-none-eabi.cmake -- cross toolchain for STM32F103C8T6 (Cortex-M3)
#
# Used via CMakePresets.json ("toolchainFile") or:
#   cmake -B build/debug -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
#
# All paths use forward slashes on purpose (CMake requirement on Windows).
# =============================================================================

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

# --- Toolchain location ------------------------------------------------------
set(HTOS_TOOLCHAIN_ROOT "E:/arm-gnu-toolchain-14.2.rel1-mingw-w64-i686-arm-none-eabi"
    CACHE PATH "Root of the GNU Arm Embedded toolchain")
set(HTOS_TOOLCHAIN_BIN "${HTOS_TOOLCHAIN_ROOT}/bin")

# Only set a tool when the caller did not pass -DCMAKE_<TOOL>, so CI (or another
# machine) can override the toolchain path without editing this file.
if(NOT DEFINED CMAKE_C_COMPILER)
  set(CMAKE_C_COMPILER   "${HTOS_TOOLCHAIN_BIN}/arm-none-eabi-gcc.exe")
endif()
if(NOT DEFINED CMAKE_CXX_COMPILER)
  set(CMAKE_CXX_COMPILER "${HTOS_TOOLCHAIN_BIN}/arm-none-eabi-g++.exe")
endif()
if(NOT DEFINED CMAKE_ASM_COMPILER)
  set(CMAKE_ASM_COMPILER "${HTOS_TOOLCHAIN_BIN}/arm-none-eabi-gcc.exe")
endif()
if(NOT DEFINED CMAKE_AR)
  set(CMAKE_AR           "${HTOS_TOOLCHAIN_BIN}/arm-none-eabi-ar.exe")
endif()
if(NOT DEFINED CMAKE_RANLIB)
  set(CMAKE_RANLIB       "${HTOS_TOOLCHAIN_BIN}/arm-none-eabi-ranlib.exe")
endif()
if(NOT DEFINED CMAKE_OBJCOPY)
  set(CMAKE_OBJCOPY      "${HTOS_TOOLCHAIN_BIN}/arm-none-eabi-objcopy.exe")
endif()
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP      "${HTOS_TOOLCHAIN_BIN}/arm-none-eabi-objdump.exe")
endif()
if(NOT DEFINED CMAKE_SIZE)
  set(CMAKE_SIZE         "${HTOS_TOOLCHAIN_BIN}/arm-none-eabi-size.exe")
endif()
if(NOT DEFINED CMAKE_NM)
  set(CMAKE_NM           "${HTOS_TOOLCHAIN_BIN}/arm-none-eabi-nm.exe")
endif()
if(NOT DEFINED CMAKE_STRIP)
  set(CMAKE_STRIP        "${HTOS_TOOLCHAIN_BIN}/arm-none-eabi-strip.exe")
endif()

# --- Sanity check ------------------------------------------------------------
if(NOT EXISTS "${CMAKE_C_COMPILER}")
  message(FATAL_ERROR
    "arm-none-eabi-gcc not found at:\n  ${CMAKE_C_COMPILER}\n"
    "Install the GNU Arm toolchain there, or configure with\n"
    "  -DHTOS_TOOLCHAIN_ROOT=<path-to-toolchain>   (or -DCMAKE_C_COMPILER=<path>)")
endif()

# A bare-metal target cannot link a normal executable during compiler probing
# (no default linker script, no libc start-up files).
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# C11, no GNU extensions -> "-std=c11" (see CMAKE_C_EXTENSIONS).
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# --- Per-build-type flags ----------------------------------------------------
#   Debug   : -Og -g3   <- exactly what the target compile contract uses
#   Release : -O2       <- NDEBUG is deliberately NOT defined, so kernel
#                          assertions stay active in every build type.
# Seeded into the cache from the toolchain file (i.e. before project() loads the
# compiler modules) because a CACHE set in CMakeLists.txt runs too late: CMake
# has already created CMAKE_C_FLAGS_DEBUG with its own default. An explicit
# -DCMAKE_C_FLAGS_DEBUG=... on the command line still wins (CACHE, no FORCE).
set(CMAKE_C_FLAGS_DEBUG   "-Og -g3" CACHE STRING "Debug C flags")
set(CMAKE_C_FLAGS_RELEASE "-O2"     CACHE STRING "Release C flags")
set(CMAKE_ASM_FLAGS_DEBUG   "-g3" CACHE STRING "Debug ASM flags")
set(CMAKE_ASM_FLAGS_RELEASE ""    CACHE STRING "Release ASM flags")
# Any other single-config type keeps asserts and stays size-conscious.
set(CMAKE_C_FLAGS_MINSIZEREL     "-Os"     CACHE STRING "MinSizeRel C flags")
set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O2 -g3" CACHE STRING "RelWithDebInfo C flags")


# Architecture flags must also apply when assembling start-up code.
set(CMAKE_ASM_FLAGS_INIT "-mcpu=cortex-m3 -mthumb")

# ...and when LINKING.  Without -mthumb on the link line the gcc driver selects
# the ARM-state newlib multilib, so __libc_init_array/memcpy come from libc in
# A32 encoding.  Cortex-M3 is Thumb-only, so the very first call out of
# Reset_Handler faults (observed: HardFault, CFSR=0x8200 PRECISERR+BFARVALID,
# LR=Reset_Handler+0x2f inside __libc_init_array).
set(CMAKE_EXE_LINKER_FLAGS_INIT "-mcpu=cortex-m3 -mthumb")

# Never look for headers/libraries outside the toolchain.
set(CMAKE_FIND_ROOT_PATH "${HTOS_TOOLCHAIN_ROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
