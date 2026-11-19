# =============================================================================
# htOS -- RTOS kernel build system
#
#   Target : STM32F103C8T6  (Cortex-M3, 72 MHz, 64 KB FLASH / 20 KB SRAM)
#   Flasher: ST-Link v2 over SWD (OpenOCD) -- see openocd.cfg
#
# Quick start (Ninja + presets):
#   cmake --preset debug
#   cmake --build --preset debug
#   cmake --build --preset debug --target size
#
# Artefacts (build/<preset>/): htOS.elf, htOS.bin, htOS.hex, htOS.map
#
# Layout note: kernel/, include/, portable/, vendor/, tests/ are external
# sources and are consumed strictly read-only by this file.
# =============================================================================
cmake_minimum_required(VERSION 3.21)

project(htOS
  VERSION     0.1.0
  DESCRIPTION "htOS RTOS kernel for STM32F103C8T6"
  LANGUAGES   C ASM)

# --- Build type default ------------------------------------------------------
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type (Debug|Release)" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release)
endif()

# --- Language standard -------------------------------------------------------
set(CMAKE_C_STANDARD          11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS        OFF)   # -std=c11, not -std=gnu11

# Optimisation lives in the per-config flags (never in target compile options),
# so -Og (Debug / compile contract) and -O2 (Release) cannot fight each other.
# They are seeded by CMAKE_C_FLAGS_<TYPE>_INIT in cmake/arm-none-eabi.cmake;
# NDEBUG is deliberately not defined, so kernel assertions stay active.

# =============================================================================
# Paths (read-only external trees)
# =============================================================================
set(HTOS_ROOT   "${CMAKE_CURRENT_SOURCE_DIR}")
set(HTOS_VENDOR "${HTOS_ROOT}/vendor")

set(HTOS_CMSIS_CORE_INC   "${HTOS_VENDOR}/CMSIS/Core/Include")
set(HTOS_CMSIS_DEVICE_INC "${HTOS_VENDOR}/CMSIS/Device/ST/STM32F1xx/Include")
set(HTOS_HAL_INC          "${HTOS_VENDOR}/STM32F1xx_HAL_Driver/Inc")
set(HTOS_HAL_SRC          "${HTOS_VENDOR}/STM32F1xx_HAL_Driver/Src")

set(HTOS_SYSTEM_SOURCE
    "${HTOS_VENDOR}/CMSIS/Device/ST/STM32F1xx/Source/Templates/system_stm32f1xx.c")
set(HTOS_STARTUP_SOURCE
    "${HTOS_VENDOR}/CMSIS/Device/ST/STM32F1xx/Source/Templates/gcc/startup_stm32f103xb.s")

# Linker script: 64 KB FLASH / 20 KB RAM for the STM32F103C8Tx.
# READ-ONLY: never edited by the build; the map file/probe never override it.
set(HTOS_LINKER_SCRIPT "${HTOS_VENDOR}/linker/STM32F103C8TX_FLASH.ld")

foreach(_required_file IN ITEMS
    "${HTOS_SYSTEM_SOURCE}"
    "${HTOS_STARTUP_SOURCE}"
    "${HTOS_LINKER_SCRIPT}"
    "${HTOS_CMSIS_CORE_INC}/core_cm3.h"
    "${HTOS_CMSIS_DEVICE_INC}/stm32f103xb.h")
  if(NOT EXISTS "${_required_file}")
    message(FATAL_ERROR "Required toolchain/vendor file is missing: ${_required_file}")
  endif()
endforeach()

# =============================================================================
# Sources
#   GLOB CONFIGURE_DEPENDS is used for the project-owned trees so that files
#   added by other engineers are picked up by the next build without a manual
#   re-configure.
# =============================================================================
file(GLOB HTOS_KERNEL_SOURCES CONFIGURE_DEPENDS "${HTOS_ROOT}/kernel/*.c")
file(GLOB HTOS_PORT_SOURCES   CONFIGURE_DEPENDS "${HTOS_ROOT}/portable/Cortex-M/*.c")
file(GLOB HTOS_BOARD_SOURCES  CONFIGURE_DEPENDS "${HTOS_ROOT}/board/*.c")
file(GLOB HTOS_APP_SOURCES    CONFIGURE_DEPENDS "${HTOS_ROOT}/app/*.c")

# HAL drivers -- explicit list, per the target compile contract:
#   stm32f1xx_hal.c, _hal_cortex.c, _hal_rcc.c, _hal_rcc_ex.c, _hal_gpio.c,
#   _hal_gpio_ex.c, _hal_uart.c, _hal_pwr.c
# The vendor tree must NOT be globbed: it also ships CubeMX *template* sources
# (stm32f1xx_hal_msp_template.c, stm32f1xx_hal_timebase_*_template.c) and LL
# drivers that are not part of this firmware.
set(HTOS_HAL_DRIVER_SOURCES
  "${HTOS_HAL_SRC}/stm32f1xx_hal.c"
  "${HTOS_HAL_SRC}/stm32f1xx_hal_cortex.c"
  "${HTOS_HAL_SRC}/stm32f1xx_hal_rcc.c"
  "${HTOS_HAL_SRC}/stm32f1xx_hal_rcc_ex.c"
  "${HTOS_HAL_SRC}/stm32f1xx_hal_gpio.c"
  "${HTOS_HAL_SRC}/stm32f1xx_hal_gpio_ex.c"
  "${HTOS_HAL_SRC}/stm32f1xx_hal_uart.c"
  "${HTOS_HAL_SRC}/stm32f1xx_hal_pwr.c"
  "${HTOS_HAL_SRC}/stm32f1xx_hal_iwdg.c"
  # stm32f1xx_hal_dma.c is required in addition: stm32f1xx_hal_uart.h refers to
  # DMA_HandleTypeDef unconditionally, so HAL_UART_MODULE_ENABLED cannot be
  # used without the DMA module. If HAL_DMA_MODULE_ENABLED is not defined in
  # stm32f1xx_hal_conf.h this file compiles to an empty object and costs no ROM.
  "${HTOS_HAL_SRC}/stm32f1xx_hal_dma.c"
  # EXTI is likewise referenced by board-level GPIO/interrupt code; it is
  # module-guarded, so it costs nothing when HAL_EXTI_MODULE_ENABLED is off.
  "${HTOS_HAL_SRC}/stm32f1xx_hal_exti.c")

foreach(_hal_source IN LISTS HTOS_HAL_DRIVER_SOURCES)
  if(NOT EXISTS "${_hal_source}")
    message(FATAL_ERROR "Required HAL source is missing: ${_hal_source}")
  endif()
endforeach()

list(LENGTH HTOS_KERNEL_SOURCES _n_kernel)
list(LENGTH HTOS_PORT_SOURCES   _n_port)
list(LENGTH HTOS_BOARD_SOURCES  _n_board)
list(LENGTH HTOS_APP_SOURCES    _n_app)
message(STATUS "htOS sources: kernel=${_n_kernel} portable=${_n_port} board=${_n_board} app=${_n_app}")
if(_n_app EQUAL 0)
  message(STATUS "htOS: no app/*.c yet (application entry point not present)")
endif()
if(_n_board EQUAL 0)
  message(STATUS "htOS: no board/*.c yet (board support not present)")
endif()

set(HTOS_SOURCES
  ${HTOS_KERNEL_SOURCES}
  ${HTOS_PORT_SOURCES}
  ${HTOS_BOARD_SOURCES}
  ${HTOS_APP_SOURCES}
  ${HTOS_HAL_DRIVER_SOURCES}
  "${HTOS_SYSTEM_SOURCE}")

# =============================================================================
# Target
# =============================================================================
add_executable(htOS ${HTOS_SOURCES} "${HTOS_STARTUP_SOURCE}")

set_target_properties(htOS PROPERTIES
  OUTPUT_NAME   "htOS"
  SUFFIX        ".elf"
  PREFIX        ""
  LINK_DEPENDS  "${HTOS_LINKER_SCRIPT}"    # relink when the script changes
  C_STANDARD    11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS  OFF)

# --- Include directories (order matches the target compile contract) ----------
target_include_directories(htOS PRIVATE
  "${HTOS_ROOT}/include"
  "${HTOS_ROOT}/kernel"
  "${HTOS_ROOT}/portable/Cortex-M"
  "${HTOS_ROOT}/board"
  "${HTOS_ROOT}/app"
  "${HTOS_CMSIS_CORE_INC}"
  "${HTOS_CMSIS_DEVICE_INC}"
  "${HTOS_HAL_INC}")

# --- Heap size ---------------------------------------------------------------
# app/ builds every task from static storage, so the reference firmware needs no
# heap at all: default 0 removes the whole TLSF pool (g_tlsf_heap) from .bss.
# Pass -DHTOS_HEAP_BYTES=12288 (or any non-zero size) to keep a heap for
# dynamic htTaskCreate()/htQueueCreate() calls in application code. The library
# default in include/htconfig.h stays 12 KB; this variable only overrides it.
set(HTOS_HEAP_BYTES 0 CACHE STRING "TLSF heap size in bytes for the firmware (0 = pure static)")

# --- Task table --------------------------------------------------------------
# HTOS_STATS_FORMATTING=ON compiles htTaskList() in and makes app/ print the
# table once at boot. It costs about 680 bytes of Flash and nothing in RAM
# (the table buffer lives on the caller's stack), so it stays off by default.
option(HTOS_STATS_FORMATTING "Compile htTaskList() and print it at boot" OFF)

# --- Compile definitions -----------------------------------------------------
target_compile_definitions(htOS PRIVATE
  STM32F103xB
  USE_HAL_DRIVER
  configTOTAL_HEAP_SIZE=${HTOS_HEAP_BYTES})

if(HTOS_STATS_FORMATTING)
  target_compile_definitions(htOS PRIVATE configUSE_STATS_FORMATTING_FUNCTIONS=1)
endif()

# --- Compile flags -----------------------------------------------------------
target_compile_options(htOS PRIVATE
  -mcpu=cortex-m3
  -mthumb
  -ffunction-sections
  -fdata-sections
  $<$<COMPILE_LANGUAGE:C>:-Wall>
  $<$<COMPILE_LANGUAGE:C>:-Wextra>
  $<$<COMPILE_LANGUAGE:C>:-Werror>)

# --- Link flags --------------------------------------------------------------
target_link_options(htOS PRIVATE
  "-T${HTOS_LINKER_SCRIPT}"
  --specs=nano.specs
  --specs=nosys.specs
  -Wl,--gc-sections
  # binutils >= 2.39 warns "LOAD segment with RWX permissions" for the ST
  # linker script (one LOAD segment spans FLASH .text and RAM .data). The
  # warning is structural to the vendor script and cannot be fixed without
  # editing it, which the build must never do.
  -Wl,--no-warn-rwx-segments
  "-Wl,-Map=${CMAKE_BINARY_DIR}/htOS.map"
  -Wl,--print-memory-usage)

# =============================================================================
# ROM budget: .text + .data must stay below 60 KB (headroom under 64 KB)
# =============================================================================
set(HTOS_ROM_BUDGET_BYTES 61440 CACHE STRING "ROM budget in bytes (.text+.data)")
set(HTOS_RAM_BUDGET_BYTES 20480 CACHE STRING "RAM budget in bytes (.data+.bss)")
set(HTOS_SIZE_DIR "${HTOS_ROOT}/build" CACHE PATH "Where size.txt is recorded")
message(STATUS "htOS ROM budget: ${HTOS_ROM_BUDGET_BYTES} bytes of 65536 (64 KB)")

# =============================================================================
# Artefacts: .bin / .hex + size report (runs after every successful link)
# =============================================================================
set(HTOS_SIZE_REPORT_SCRIPT "${HTOS_ROOT}/scripts/size-report.cmake")
set(HTOS_BIN "${CMAKE_BINARY_DIR}/htOS.bin")
set(HTOS_HEX "${CMAKE_BINARY_DIR}/htOS.hex")

add_custom_command(TARGET htOS POST_BUILD
  COMMAND "${CMAKE_OBJCOPY}" -O binary "$<TARGET_FILE:htOS>" "${HTOS_BIN}"
  COMMAND "${CMAKE_OBJCOPY}" -O ihex   "$<TARGET_FILE:htOS>" "${HTOS_HEX}"
  COMMAND "${CMAKE_COMMAND}"
          "-DHTOS_ELF=$<TARGET_FILE:htOS>"
          "-DHTOS_SIZE_TOOL=${CMAKE_SIZE}"
          "-DHTOS_ROM_BUDGET=${HTOS_ROM_BUDGET_BYTES}"
          "-DHTOS_RAM_BUDGET=${HTOS_RAM_BUDGET_BYTES}"
          "-DHTOS_SIZE_DIR=${HTOS_SIZE_DIR}"
          "-DHTOS_BUILD_NAME=${CMAKE_BUILD_TYPE}"
          -P "${HTOS_SIZE_REPORT_SCRIPT}"
  BYPRODUCTS "${HTOS_BIN}" "${HTOS_HEX}"
  COMMENT "objcopy -> htOS.bin / htOS.hex, then ROM budget check")

# --- `size` target -----------------------------------------------------------
add_custom_target(size
  COMMAND "${CMAKE_COMMAND}"
          "-DHTOS_ELF=$<TARGET_FILE:htOS>"
          "-DHTOS_SIZE_TOOL=${CMAKE_SIZE}"
          "-DHTOS_ROM_BUDGET=${HTOS_ROM_BUDGET_BYTES}"
          "-DHTOS_RAM_BUDGET=${HTOS_RAM_BUDGET_BYTES}"
          "-DHTOS_SIZE_DIR=${HTOS_SIZE_DIR}"
          "-DHTOS_BUILD_NAME=${CMAKE_BUILD_TYPE}"
          -P "${HTOS_SIZE_REPORT_SCRIPT}"
  DEPENDS htOS
  USES_TERMINAL
  COMMENT "htOS firmware size report (text/data/bss)")

message(STATUS "htOS: build type=${CMAKE_BUILD_TYPE} generator=${CMAKE_GENERATOR}")
message(STATUS "htOS: artefacts -> ${CMAKE_BINARY_DIR}/htOS.elf|.bin|.hex|.map")
