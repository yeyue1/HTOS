/*
 * Copyright (c) 2025, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2025-08-16     Yeyue       first version
 */

#include <stdbool.h>
#include <stdint.h>
#ifdef PKG_USING_MCOREDUMP_FILESYSTEM
#include <dfs_file.h>
#include <sys/time.h>
#include <unistd.h>

#endif /* PKG_USING_MCOREDUMP_FILESYSTEM */
#include "coredump.h"

#define COREDUMP_MEMORY_MAGIC (0x434D4450) /* "CMDP" */

/* Static memory buffer for coredump storage */
#ifndef PKG_MCOREDUMP_MEMORY_SIZE
#define COREDUMP_MEMORY_SIZE (1) /* Default 8KB buffer size */
#else
#define COREDUMP_MEMORY_SIZE PKG_MCOREDUMP_MEMORY_SIZE
#endif

typedef struct {
  uint32_t magic;     /* Magic number to identify valid coredump */
  uint32_t data_size; /* Actual coredump data size */
  uint32_t crc32;     /* CRC32 checksum */
  uint8_t data[COREDUMP_MEMORY_SIZE]; /* Coredump data buffer */
} coredump_memory_t;

/* Static memory buffer for coredump storage - must persist across resets */
#if defined(__ARMCC_VERSION) && (__ARMCC_VERSION >= 6010050)
/* ARM Compiler V6 (armclang) - use custom section with UNINIT attribute in
 * scatter file */
static coredump_memory_t coredump_memory_buffer
    __attribute__((section(".bss.NoInit")));
#elif defined(__GNUC__)
/* GCC - use .noinit section (not initialized by startup code) */
static coredump_memory_t coredump_memory_buffer
    __attribute__((section(".noinit")));
#elif defined(__ARMCC_VERSION) && (__ARMCC_VERSION < 6010050)
/* ARM Compiler 5 (armcc) - use custom section */
static coredump_memory_t coredump_memory_buffer
    __attribute__((section(".bss.NoInit"), zero_init));
#else
/* Fallback for other compilers - normal static allocation */
#warning                                                                       \
    "MCoreDump: Unknown compiler, coredump buffer may not persist across resets"
#warning "MCoreDump: Data will be lost after system reset"
static coredump_memory_t coredump_memory_buffer;
#endif

/* Memory write context */
static struct {
  uint32_t offset;
  bool overflow;
} memory_write_ctx;

#ifdef PKG_USING_MCOREDUMP_FILESYSTEM
/* Internal function declarations */
static int create_coredump_filename(void);
static int prepare_coredump_filesystem(void);
#endif /* PKG_USING_MCOREDUMP_FILESYSTEM */

/* Global variables for serial output */
static uint32_t serial_crc32;
static uint32_t serial_byte_count;

#ifdef PKG_USING_MCOREDUMP_FILESYSTEM
/* Global variables for filesystem output */
static int coredump_fd = -1;
static char coredump_filename[64];

#ifndef PKG_MCOREDUMP_FILESYSTEM_DIR
#define COREDUMP_DIR "/sdcard"
#else
#define COREDUMP_DIR PKG_MCOREDUMP_FILESYSTEM_DIR
#endif

#ifndef PKG_MCOREDUMP_FILESYSTEM_PREFIX
#define COREDUMP_PREFIX "core_"
#else
#define COREDUMP_PREFIX PKG_MCOREDUMP_FILESYSTEM_PREFIX
#endif

#define COREDUMP_EXT ".elf"
#endif /* PKG_USING_MCOREDUMP_FILESYSTEM */

typedef struct {
  uint16_t magic;
  uint16_t page_size;
  uint32_t file_size;
  uint8_t corefile[1];
} persistent_format_t;

static uint32_t mcd_crc32b(const uint8_t *message, int32_t megLen,
                           uint32_t initCrc) {
  int i, j;
  uint32_t byte, crc, mask;

  i = 0;
  crc = initCrc;
  for (i = 0; i < megLen; i++) {
    byte = message[i]; /* Get next byte. */
    crc = crc ^ byte;
    for (j = 7; j >= 0; j--) /* Do eight times. */
    {
      mask = -(crc & 1);
      crc = (crc >> 1) ^ (0xEDB88320 & mask);
    }
  }

  return ~crc;
}

static void corefile_serial_write(uint8_t *data, int len) {
  for (int i = 0; i < len; i++) {
    uint8_t b = data[i];
    serial_crc32 = mcd_crc32b(&b, 1, serial_crc32) ^ 0xFFFFFFFF;
    mcd_print("%02x", b);
    serial_byte_count++;
  }
}

static void corefile_memory_write(uint8_t *data, int len) {
  if (memory_write_ctx.overflow)
    return;

  /* Check if we have enough space */
  if (memory_write_ctx.offset + len > COREDUMP_MEMORY_SIZE) {
    memory_write_ctx.overflow = true;
    mcd_println("WARNING: Coredump memory buffer overflow! Data truncated.");
    len = COREDUMP_MEMORY_SIZE - memory_write_ctx.offset;
    if (len <= 0)
      return;
  }

  /* Copy data to memory buffer */
  mcd_memcpy(&coredump_memory_buffer.data[memory_write_ctx.offset], data, len);
  memory_write_ctx.offset += len;
}

/**
 * @brief Check if there's a valid coredump in memory buffer
 *
 * @return bool true if valid coredump exists, false otherwise
 */
mcd_bool_t mcd_check_memory_coredump(void) {
  return (coredump_memory_buffer.magic == COREDUMP_MEMORY_MAGIC &&
          coredump_memory_buffer.data_size > 0 &&
          coredump_memory_buffer.data_size <= COREDUMP_MEMORY_SIZE);
}

/**
 * @brief Print coredump memory information at startup
 *
 * This function should be called during system initialization
 */
void mcd_print_memoryinfo(void) {
  mcd_print("\n=== MCoreDump Memory Check ===\n");
  mcd_print("Memory buffer address: 0x%08X\n",
            (uint32_t)&coredump_memory_buffer);
  mcd_print("Buffer size: %d bytes\n", sizeof(coredump_memory_t));

  if (mcd_check_memory_coredump()) {
    mcd_print("*** COREDUMP FOUND IN MEMORY ***\n");
    mcd_print("Data size: %d bytes\n", coredump_memory_buffer.data_size);
#ifdef PKG_USING_MCOREDUMP_FILESYSTEM
    mcd_print("Use 'mcd_dump_filesystem' command to save to filesystem.\n");
#endif /* PKG_USING_MCOREDUMP_FILESYSTEM */
    mcd_print("Use 'mcd_dump_memory' command to dump memory to terminal.\n");
  } else {
    mcd_print("No valid coredump found in memory.\n");
  }
  mcd_print("============================\n\n");
}

#ifdef PKG_USING_MCOREDUMP_FILESYSTEM
/**
 * @brief Save coredump from memory buffer to filesystem
 *
 * @return int MCD_OK on success, error code on failure
 */
int mcd_dump_filesystem(void) {
  if (!mcd_check_memory_coredump()) {
    mcd_print("No valid coredump found in memory buffer.\n");
    return MCD_ERROR;
  }

  /* Verify CRC32 */
  uint32_t calculated_crc =
      mcd_crc32b(coredump_memory_buffer.data, coredump_memory_buffer.data_size,
                 0xFFFFFFFF) ^
      0xFFFFFFFF;

  if (calculated_crc != coredump_memory_buffer.crc32) {
    mcd_print("ERROR: Coredump data corruption detected (CRC mismatch)!\n");
    mcd_print("Expected: 0x%08X, Calculated: 0x%08X\n",
              coredump_memory_buffer.crc32, calculated_crc);
    return MCD_ERROR;
  }

  /* Prepare filesystem */
  if (prepare_coredump_filesystem() != 0) {
    mcd_print("ERROR: Failed to prepare filesystem for coredump\n");
    return MCD_ERROR;
  }

  mcd_print("Saving coredump from memory to file: %s\n", coredump_filename);

  /* Write coredump data directly (pure ELF format without custom header) */
  if (write(coredump_fd, coredump_memory_buffer.data,
            coredump_memory_buffer.data_size) < 0) {
    mcd_print("ERROR: Failed to write coredump data\n");
    close(coredump_fd);
    return MCD_ERROR;
  }

  close(coredump_fd);
  coredump_fd = -1;

  mcd_print("Coredump saved successfully to: %s\n", coredump_filename);

  /* Clear memory buffer after successful save */
  mcd_memset(&coredump_memory_buffer, 0, sizeof(coredump_memory_t));

  return MCD_OK;
}
MCD_CMD_EXPORT(mcd_dump_filesystem, Save coredump from memory to filesystem);

static int create_coredump_filename(void) {
  struct timeval tv;
  struct tm *tm_info;

  /* Get current time */
  if (gettimeofday(&tv, NULL) != 0) {
    /* If time is not available, use a simple counter-based name */
    static int file_counter = 0;
    file_counter++;
    snprintf(coredump_filename, sizeof(coredump_filename), "%s/%s%04d%s",
             COREDUMP_DIR, COREDUMP_PREFIX, file_counter, COREDUMP_EXT);
  } else {
    tm_info = localtime(&tv.tv_sec);

    /* Create filename with timestamp */
    snprintf(coredump_filename, sizeof(coredump_filename),
             "%s/%s%04d%02d%02d_%02d%02d%02d%s", COREDUMP_DIR, COREDUMP_PREFIX,
             tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
             tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec, COREDUMP_EXT);
  }

  return 0;
}

static int prepare_coredump_filesystem(void) {
  struct stat st;

  /* Check if coredump directory exists first */
  if (stat(COREDUMP_DIR, &st) != 0) {
    mcd_print("ERROR: %s directory not found. Please mount filesystem first.\n",
              COREDUMP_DIR);
    return -1;
  }

  /* Create coredump directory if it doesn't exist */
  if (stat(COREDUMP_DIR, &st) != 0) {
    mcd_print("Creating directory: %s\n", COREDUMP_DIR);
    if (mkdir(COREDUMP_DIR, 0x777) != 0) {
      mcd_print("ERROR: Failed to create coredump directory: %s\n",
                COREDUMP_DIR);
      mcd_print("Please check if filesystem is mounted and writable.\n");
      return -1;
    }
    mcd_print("Directory created successfully: %s\n", COREDUMP_DIR);
  }

  /* Generate filename */
  create_coredump_filename();

  mcd_print("Creating coredump file: %s\n", coredump_filename);

  /* Open file for writing */
  coredump_fd = open(coredump_filename, O_WRONLY | O_CREAT);
  if (coredump_fd < 0) {
    mcd_print("ERROR: Failed to create coredump file: %s\n", coredump_filename);
    return -1;
  }

  mcd_print("Coredump file opened successfully (fd: %d)\n", coredump_fd);
  return 0;
}
#endif /* PKG_USING_MCOREDUMP_FILESYSTEM */

/**
 * @brief Core dump with output mode selection
 *
 * @param output_mode MCD_OUTPUT_FLASH for flash storage, MCD_OUTPUT_SERIAL for
 * serial output, MCD_OUTPUT_MEMORY for memory storage
 * @return int MCD_OK on success, error code on failure
 */
int mcd_faultdump_ex(mcd_output_mode_t output_mode) {
  struct thread_info_ops ops;

  if (output_mode == MCD_OUTPUT_SERIAL) {
    mcd_init(corefile_serial_write);

    mcd_irq_state_t irq_save = mcd_irq_disable();

    serial_crc32 = 0xFFFFFFFF;

    mcd_print("coredump start : {\n");
    mcd_multi_dump();
    mcd_print("\n} coredump end\n");

    serial_crc32 = ~serial_crc32;

    mcd_print("crc32 : %08x\n", serial_crc32);

    mcd_irq_enable(irq_save);
  } else if (output_mode == MCD_OUTPUT_MEMORY) {
    /* Memory buffer output mode - for exception handling */
    mcd_print("\n=== MCoreDump Memory Storage Started ===\n");

    /* Initialize memory write context */
    memory_write_ctx.offset = 0;
    memory_write_ctx.overflow = false;

    /* Clear memory buffer */
    mcd_memset(&coredump_memory_buffer, 0, sizeof(coredump_memory_t));

    /* Initialize mcd with memory write function */
    mcd_init(corefile_memory_write);

    /* Generate and write coredump to memory */
    mcd_rtos_thread_ops(&ops);
    mcd_gen_coredump(&ops);

    /* Finalize memory buffer */
    if (!memory_write_ctx.overflow && memory_write_ctx.offset > 0) {
      coredump_memory_buffer.magic = COREDUMP_MEMORY_MAGIC;
      coredump_memory_buffer.data_size = memory_write_ctx.offset;
      coredump_memory_buffer.crc32 =
          mcd_crc32b(coredump_memory_buffer.data, memory_write_ctx.offset,
                     0xFFFFFFFF) ^
          0xFFFFFFFF;

      mcd_println("Coredump saved to memory buffer:\n");
      mcd_println("  Address: 0x%08X", (uint32_t)&coredump_memory_buffer);
      mcd_println("  Size: %d bytes", memory_write_ctx.offset);
      mcd_println("  CRC32: 0x%08X", coredump_memory_buffer.crc32);
    } else {
      mcd_println("ERROR: Memory buffer overflow or no data written");
      mcd_println("=== MCoreDump Memory Storage Failed ===\n");
      return MCD_ERROR;
    }
  }

  mcd_println("=== MCoreDump Memory Storage Completed ===\n");

  return MCD_OK;
}

/**
 * @brief Generate coredump with specified output mode
 *
 * This is the main entry point for generating coredumps in MCoreDump system.
 * It supports multiple output modes to accommodate different debugging
 * scenarios:
 *
 * - MCD_OUTPUT_MEMORY: Saves coredump to static memory buffer (persistent
 * across resets)
 * - MCD_OUTPUT_SERIAL: Outputs coredump data via serial port in hex format
 * - MCD_OUTPUT_FILESYSTEM: Saves coredump to filesystem as ELF file (if
 * filesystem support is enabled)
 *
 * The function is designed to be called from exception handlers, assert hooks,
 * or user applications when fault analysis is needed.
 *
 * @param output_mode Output destination for coredump data:
 *                    - MCD_OUTPUT_MEMORY: Store in memory buffer (recommended
 * for fault handlers)
 *                    - MCD_OUTPUT_SERIAL: Output via serial port (for immediate
 * analysis)
 *                    - MCD_OUTPUT_FILESYSTEM: Save to file (requires filesystem
 * support)
 *
 * @return int Status code:
 *             - MCD_OK: Coredump generated successfully
 *             - MCD_ERROR: Failed to generate coredump (memory overflow,
 * filesystem error, etc.)
 *
 * @note This function calls mcd_faultdump_ex() internally, which contains the
 * actual implementation. The memory buffer mode is particularly useful for hard
 * fault handlers as the data persists across system resets when power is
 * maintained.
 *
 * @warning When called from interrupt context (e.g., hard fault handler), avoid
 * using filesystem mode as it may involve complex I/O operations.
 *
 * @see mcd_faultdump_ex() for detailed implementation
 * @see mcd_check_memory_coredump() to check for existing coredumps in memory
 * @see mcd_dump_filesystem() to save memory coredump to filesystem
 */
int mcd_faultdump(mcd_output_mode_t output_mode) {
  /* Default to memory storage mode for exception handling */
  return mcd_faultdump_ex(output_mode);
}

/**
 * @brief Print coredump from memory buffer via serial output
 *
 * This function reads coredump data from memory buffer and prints it via serial
 * port
 *
 * @return int MCD_OK on success, error code on failure
 */
static int mcd_dump_memory(void) {
  if (!mcd_check_memory_coredump()) {
    mcd_print("No valid coredump found in memory buffer.\n");
    return MCD_ERROR;
  }

  /* Verify CRC32 */
  uint32_t calculated_crc =
      mcd_crc32b(coredump_memory_buffer.data, coredump_memory_buffer.data_size,
                 0xFFFFFFFF) ^
      0xFFFFFFFF;

  if (calculated_crc != coredump_memory_buffer.crc32) {
    mcd_print("ERROR: Coredump data corruption detected (CRC mismatch)!\n");
    mcd_print("Expected: 0x%08X, Calculated: 0x%08X\n",
              coredump_memory_buffer.crc32, calculated_crc);
    return MCD_ERROR;
  }

  mcd_print("\n=== MCoreDump Memory Data Serial Output ===\n");
  mcd_print("Coredump found in memory buffer:\n");
  mcd_print("  Address: 0x%08X\n", (uint32_t)&coredump_memory_buffer);
  mcd_print("  Size: %d bytes\n", coredump_memory_buffer.data_size);
  mcd_print("  CRC32: 0x%08X\n", coredump_memory_buffer.crc32);
  mcd_print("\n");

  /* Calculate and display CRC32 for serial output */
  uint32_t serial_crc = 0xFFFFFFFF;
  uint32_t byte_count = 0;

  mcd_print("coredump start : {\n");

  /* Print coredump data in hex format */
  for (uint32_t i = 0; i < coredump_memory_buffer.data_size; i++) {
    uint8_t b = coredump_memory_buffer.data[i];
    serial_crc = mcd_crc32b(&b, 1, serial_crc) ^ 0xFFFFFFFF;
    mcd_print("%02x", b);
    byte_count++;
  }

  mcd_print("\n} coredump end\n");

  serial_crc = ~serial_crc;
  mcd_print("crc32 : %08x\n", serial_crc);
  mcd_print("bytes : %d\n", byte_count);
  mcd_print("=== MCoreDump Memory Data Serial Output Completed ===\n\n");

  return MCD_OK;
}
MCD_CMD_EXPORT(mcd_dump_memory, Print memory coredump via serial);
