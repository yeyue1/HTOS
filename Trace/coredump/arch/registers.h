/*
 * Copyright (c) 2025, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2025-08-16     Rbb666       first version
 */

#ifndef __REGISTERS_H__
#define __REGISTERS_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct armv7m_core_regset
{
    uint32_t r0;
    uint32_t r1;
    uint32_t r2;
    uint32_t r3;
    uint32_t r4;
    uint32_t r5;
    uint32_t r6;
    uint32_t r7;
    uint32_t r8;
    uint32_t r9;
    uint32_t r10;
    uint32_t r11;
    uint32_t r12;
    uint32_t sp;
    uint32_t lr;
    uint32_t pc;
    uint32_t xpsr;
};

struct arm_vfpv2_regset
{
    uint64_t d0;
    uint64_t d1;
    uint64_t d2;
    uint64_t d3;
    uint64_t d4;
    uint64_t d5;
    uint64_t d6;
    uint64_t d7;
    uint64_t d8;
    uint64_t d9;
    uint64_t d10;
    uint64_t d11;
    uint64_t d12;
    uint64_t d13;
    uint64_t d14;
    uint64_t d15;
    uint32_t fpscr;
};

typedef struct armv7m_core_regset    core_regset_type;
typedef struct arm_vfpv2_regset      fp_regset_type;

#ifdef __cplusplus
}
#endif

#endif /* __MCD_REGISTERS_H__ */
