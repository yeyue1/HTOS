/*
 * Copyright (c) 2025, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2025-08-16     Rbb666       Unified ARM architecture definitions
 */

#ifndef __MCD_ARM_DEFINE_H__
#define __MCD_ARM_DEFINE_H__

#include "registers.h"

/* ARM Architecture Detection */
#if defined(__aarch64__) || defined(_M_ARM64)
#define MCD_ARM_ARCH_64BIT          1
#define MCD_ARM_ARCH_32BIT          0
#elif defined(__arm__) || defined(_M_ARM) || defined(__ARM_ARCH)
#define MCD_ARM_ARCH_64BIT          0
#define MCD_ARM_ARCH_32BIT          1
#else
#error "Unsupported ARM architecture"
#endif

/* Architecture-specific ELF definitions */
#if MCD_ARM_ARCH_64BIT
/* ARM64 (AArch64) definitions */
#define MCOREDUMP_ELF_CLASS         ELFCLASS64
#define MCOREDUMP_ELF_ENDIAN        ELFDATA2LSB
#define MCOREDUMP_MACHINE           EM_AARCH64
#define MCOREDUMP_OSABI             ELFOSABI_NONE
#define MCOREDUMP_PRSTATUS_SIZE     392
#define MCOREDUMP_FPREGSET_SIZE     (32 * 16 + 8)
#define MCD_PRSTATUS_REG_OFFSET     112

/* Function mappings */
#define fill_note_prstatus_desc     arm64_fill_note_prstatus_desc
#define fill_note_fpregset_desc     arm64_fill_note_fpregset_desc
    
#elif MCD_ARM_ARCH_32BIT
/* ARM32 (AArch32) definitions */
#define MCOREDUMP_ELF_CLASS         ELFCLASS32
#define MCOREDUMP_ELF_ENDIAN        ELFDATA2LSB
#define MCOREDUMP_MACHINE           EM_ARM
#define MCOREDUMP_OSABI             ELFOSABI_ARM
#define MCOREDUMP_PRSTATUS_SIZE     148
#define MCOREDUMP_FPREGSET_SIZE     (32 * 8 + 4)
#define MCD_PRSTATUS_REG_OFFSET     72

/* Function mappings */
#define fill_note_prstatus_desc     arm32_fill_note_prstatus_desc
#define fill_note_fpregset_desc     arm32_fill_note_fpregset_desc
#endif /* MCD_ARM_ARCH_32BIT */

/* Function declarations */
#if MCD_ARM_ARCH_32BIT
void arm32_fill_note_prstatus_desc(uint8_t *desc, core_regset_type *regset);
void arm32_fill_note_fpregset_desc(uint8_t *desc, fp_regset_type *regset);
#endif

#if MCD_ARM_ARCH_64BIT
void arm64_fill_note_prstatus_desc(uint8_t *desc, core_regset_type *regset);
void arm64_fill_note_fpregset_desc(uint8_t *desc, fp_regset_type *regset);
#endif

#endif /* __MCD_ARM_DEFINE_H__ */
