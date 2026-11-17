# =============================================================================
# scripts/size-report.cmake -- firmware size report + ROM budget gate
#
# Invoked by CMakeLists.txt twice:
#   1. as a POST_BUILD step of the htOS target  -> fails the BUILD on budget
#      overflow (message(FATAL_ERROR) exits non-zero, so ninja/make stop).
#   2. behind the `size` target                 -> on-demand report.
#
# Inputs (all passed with -D):
#   HTOS_ELF        path to htOS.elf                                   (required)
#   HTOS_SIZE_TOOL  path to arm-none-eabi-size(.exe)                   (required)
#   HTOS_ROM_BUDGET bytes allowed for .text+.data         (default 61440)
#   HTOS_RAM_BUDGET bytes allowed for .data+.bss          (default 20480)
#   HTOS_SIZE_DIR   where size.txt / size-baseline.txt live (default <cwd>/build)
#   HTOS_BUILD_NAME free-form label recorded in the report
#   HTOS_WRITE_BASELINE  ON -> promote the current numbers to the baseline
#
# Output:
#   * a delimited block on stdout (===== HTOS SIZE BEGIN/END =====) that a
#     script can parse, containing size=<bytes> and key=value lines
#   * build/size.txt          (current report, machine readable)
#   * baseline-vs-current delta block when build/size-baseline.txt exists
# =============================================================================
cmake_minimum_required(VERSION 3.21)

# --- Input validation --------------------------------------------------------
foreach(_required IN ITEMS HTOS_ELF HTOS_SIZE_TOOL)
  if(NOT DEFINED ${_required})
    message(FATAL_ERROR "size-report: -D${_required}=<path> is required")
  endif()
endforeach()

if(NOT EXISTS "${HTOS_ELF}")
  message(FATAL_ERROR "size-report: ELF file not found: ${HTOS_ELF}")
endif()
if(NOT EXISTS "${HTOS_SIZE_TOOL}")
  message(FATAL_ERROR "size-report: size tool not found: ${HTOS_SIZE_TOOL}")
endif()

if(NOT DEFINED HTOS_ROM_BUDGET)
  set(HTOS_ROM_BUDGET 61440)
endif()
if(NOT DEFINED HTOS_RAM_BUDGET)
  set(HTOS_RAM_BUDGET 20480)
endif()
if(NOT DEFINED HTOS_SIZE_DIR)
  set(HTOS_SIZE_DIR "${CMAKE_CURRENT_BINARY_DIR}/build")
endif()
if(NOT DEFINED HTOS_BUILD_NAME)
  set(HTOS_BUILD_NAME "unknown")
endif()

# --- Run the size tool (Berkeley format: text data bss dec hex filename) -----
execute_process(
  COMMAND        "${HTOS_SIZE_TOOL}" "${HTOS_ELF}"
  RESULT_VARIABLE _size_rc
  OUTPUT_VARIABLE _size_out
  ERROR_VARIABLE  _size_err)
if(NOT _size_rc EQUAL 0)
  message(FATAL_ERROR "size-report: arm-none-eabi-size failed (${_size_rc}): ${_size_err}")
endif()

string(REGEX MATCH "[ \t\r\n]+([0-9]+)[ \t\r\n]+([0-9]+)[ \t\r\n]+([0-9]+)" _size_match "${_size_out}")
if(_size_match STREQUAL "")
  message(FATAL_ERROR "size-report: could not parse arm-none-eabi-size output: ${_size_out}")
endif()
set(_text "${CMAKE_MATCH_1}")   # .text + .isr_vector + .rodata ... (FLASH)
set(_data "${CMAKE_MATCH_2}")   # initialised data (FLASH + RAM)
set(_bss  "${CMAKE_MATCH_3}")   # zero-initialised data (RAM only)

# --- Derived numbers ---------------------------------------------------------
math(EXPR _rom "${_text} + ${_data}")     # bytes that occupy FLASH
math(EXPR _ram "${_data} + ${_bss}")      # bytes that occupy SRAM
math(EXPR _rom_pct "${_rom} * 100 / ${HTOS_ROM_BUDGET}")
math(EXPR _ram_pct "${_ram} * 100 / ${HTOS_RAM_BUDGET}")

if(_rom LESS_EQUAL HTOS_ROM_BUDGET)
  math(EXPR _rom_headroom "${HTOS_ROM_BUDGET} - ${_rom}")
  set(_rom_status "OK")
  set(_rom_verdict "within budget")
else()
  math(EXPR _rom_headroom "${_rom} - ${HTOS_ROM_BUDGET}")
  set(_rom_status "OVER")
  set(_rom_verdict "OVER BUDGET")
endif()

# --- Report block ------------------------------------------------------------
set(_report "")
string(APPEND _report "===== HTOS SIZE BEGIN =====\n")
string(APPEND _report "build=${HTOS_BUILD_NAME}\n")
string(APPEND _report "elf=${HTOS_ELF}\n")
string(APPEND _report "text=${_text}\n")
string(APPEND _report "data=${_data}\n")
string(APPEND _report "bss=${_bss}\n")
string(APPEND _report "rom=${_rom}\n")
string(APPEND _report "ram=${_ram}\n")
string(APPEND _report "rom_budget=${HTOS_ROM_BUDGET}\n")
string(APPEND _report "ram_budget=${HTOS_RAM_BUDGET}\n")
string(APPEND _report "rom_headroom=${_rom_headroom}\n")
string(APPEND _report "rom_status=${_rom_status}\n")
string(APPEND _report "-- size breakdown ---------------------------------------------\n")
string(APPEND _report ".text            : ${_text} bytes\n")
string(APPEND _report ".data            : ${_data} bytes\n")
string(APPEND _report ".bss             : ${_bss} bytes\n")
string(APPEND _report "-- memory usage -----------------------------------------------\n")
string(APPEND _report "FLASH .text+.data: ${_rom} / ${HTOS_ROM_BUDGET} bytes  (${_rom_pct}%)  budget ${_rom_verdict}\n")
string(APPEND _report "SRAM  .data+.bss : ${_ram} / ${HTOS_RAM_BUDGET} bytes  (${_ram_pct}%)\n")
string(APPEND _report "part total       : 65536 bytes FLASH / 20480 bytes SRAM\n")
string(APPEND _report "===== HTOS SIZE END =====\n")

# --- Baseline comparison -----------------------------------------------------
set(_baseline_file "${HTOS_SIZE_DIR}/size-baseline.txt")
if(EXISTS "${_baseline_file}")
  file(READ "${_baseline_file}" _baseline_text)
  foreach(_metric IN ITEMS text data bss rom ram)
    string(REGEX MATCH "${_metric}=([0-9]+)" _base_match "${_baseline_text}")
    if(NOT _base_match STREQUAL "")
      set(_base_${_metric} "${CMAKE_MATCH_1}")
    endif()
  endforeach()
  if(DEFINED _base_rom AND DEFINED _base_ram)
    # signed deltas: prefix "+" when growing, "-" is already in the number
    foreach(_metric IN ITEMS text data bss rom ram)
      math(EXPR _${_metric}_delta "${_${_metric}} - ${_base_${_metric}}")
      if(_${_metric}_delta GREATER 0)
        set(_sign_${_metric} "+${_${_metric}_delta}")
      else()
        set(_sign_${_metric} "${_${_metric}_delta}")
      endif()
    endforeach()
    string(APPEND _report "===== HTOS SIZE DELTA BEGIN =====\n")
    string(APPEND _report "baseline=${_baseline_file}\n")
    string(APPEND _report "FLASH .text+.data: ${_base_rom} -> ${_rom} bytes  (${_sign_rom})\n")
    string(APPEND _report "SRAM  .data+.bss : ${_base_ram} -> ${_ram} bytes  (${_sign_ram})\n")
    string(APPEND _report ".text            : ${_base_text} -> ${_text} bytes  (${_sign_text})\n")
    string(APPEND _report ".data            : ${_base_data} -> ${_data} bytes  (${_sign_data})\n")
    string(APPEND _report ".bss             : ${_base_bss} -> ${_bss} bytes  (${_sign_bss})\n")
    string(APPEND _report "===== HTOS SIZE DELTA END =====\n")
  endif()
else()
  string(APPEND _report "-- no baseline recorded yet; promote this build with HTOS_WRITE_BASELINE=ON\n")
endif()

# --- Emit --------------------------------------------------------------------
message("${_report}")

file(MAKE_DIRECTORY "${HTOS_SIZE_DIR}")
file(WRITE "${HTOS_SIZE_DIR}/size.txt" "${_report}")

if(HTOS_WRITE_BASELINE)
  file(WRITE "${_baseline_file}" "${_report}")
  message(STATUS "size-report: baseline updated -> ${_baseline_file}")
endif()

# --- ROM budget gate (this is what fails the build) --------------------------
if(_rom GREATER HTOS_ROM_BUDGET)
  message(FATAL_ERROR
    "ROM budget exceeded: .text+.data = ${_rom} bytes > ${HTOS_ROM_BUDGET} bytes "
    "(${_rom_headroom} bytes over). The part has 65536 bytes of FLASH; the "
    "budget keeps 4 KB of headroom. Reduce code size before shipping.")
endif()
