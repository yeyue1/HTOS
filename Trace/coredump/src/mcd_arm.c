/*
 * Copyright (c) 2025, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2025-08-16     Rbb666       Unified ARM architecture implementation
 */

#include <stdint.h>
#include "mcd_cfg.h"
#include "mcd_arm_define.h"

/* Global task ID counter for note generation */
static uint32_t task_id_counter = 3539;

#if MCD_ARM_ARCH_32BIT
/**
 * @brief Fill prstatus note description for ARM32
 * 
 * @param desc Pointer to description buffer
 * @param regset Pointer to core register set
 */
void arm32_fill_note_prstatus_desc(uint8_t *desc, core_regset_type *regset)
{
    uint16_t *signal = (uint16_t *)&desc[12];
    uint32_t *lwpid = (uint32_t *)&desc[24];

    mcd_memset(desc, 0, MCOREDUMP_PRSTATUS_SIZE);
    *signal = 0;
    *lwpid = task_id_counter++;
    
    /* Copy ARM32 registers to offset 72 */
    mcd_memcpy(desc + MCD_PRSTATUS_REG_OFFSET, regset, sizeof(core_regset_type));
}

/**
 * @brief Fill fpregset note description for ARM32
 * 
 * @param desc Pointer to description buffer
 * @param regset Pointer to floating point register set
 */
void arm32_fill_note_fpregset_desc(uint8_t *desc, fp_regset_type *regset)
{
    if (regset != NULL)
    {
        /* Copy VFP registers (32 * 8 bytes = 256 bytes) */
        mcd_memcpy(desc, regset, sizeof(fp_regset_type) - sizeof(uint32_t));
        /* Copy FPSCR at the end */
        mcd_memcpy(desc + 32 * 8, &regset->fpscr, sizeof(uint32_t));
    }
    else
    {
        mcd_memset(desc, 0, MCOREDUMP_FPREGSET_SIZE);
    }
}
#endif /* MCD_ARM_ARCH_32BIT */

#if MCD_ARM_ARCH_64BIT
/**
 * @brief Fill prstatus note description for ARM64
 * 
 * @param desc Pointer to description buffer
 * @param regset Pointer to core register set
 */
void arm64_fill_note_prstatus_desc(uint8_t *desc, core_regset_type *regset)
{
    uint16_t *signal = (uint16_t *)&desc[12];
    uint32_t *lwpid = (uint32_t *)&desc[32];

    mcd_memset(desc, 0, MCOREDUMP_PRSTATUS_SIZE);
    *signal = 0;
    *lwpid = task_id_counter++;
    
    /* Copy ARM64 registers to offset 112 */
    mcd_memcpy(desc + MCD_PRSTATUS_REG_OFFSET, regset, sizeof(core_regset_type));
}

/**
 * @brief Fill fpregset note description for ARM64
 * 
 * @param desc Pointer to description buffer  
 * @param regset Pointer to floating point register set
 */
void arm64_fill_note_fpregset_desc(uint8_t *desc, fp_regset_type *regset)
{
    if (regset != NULL)
    {
        /* Copy SIMD/FP registers (32 * 16 bytes = 512 bytes) */
        mcd_memcpy(desc, regset, sizeof(fp_regset_type) - 8);
        
        /* Copy FPSR and FPCR at the end */
        mcd_memcpy(desc + 32 * 16, &regset->fpsr, sizeof(uint32_t));
        mcd_memcpy(desc + 32 * 16 + 4, &regset->fpcr, sizeof(uint32_t));
    }
    else
    {
        mcd_memset(desc, 0, MCOREDUMP_FPREGSET_SIZE);
    }
}
#endif /* MCD_ARM_ARCH_64BIT */
