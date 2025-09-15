/*
 * Copyright (c) 2025, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2025-08-16     Rbb666       first version
 */

#include "coredump.h"
#include "registers.h"


#define FPU_CPACR 0xE000ED88


/* 新增：检查寄存器是否已经被填充（异常场景） */
int mcd_check_regset_filled(void)
{
    core_regset_type *core_regs = get_cur_core_regset_address();
    
    /* 检查关键寄存器是否已填充（非零且在合理范围） */
    if (core_regs->pc >= 0x08000000 && core_regs->pc <= 0x08080000 &&  /* PC 在 Flash 范围 */
        core_regs->sp >= 0x20000000 && core_regs->sp <= 0x20020000)    /* SP 在 RAM 范围 */
    {
        return 1;  /* 已填充 */
    }
    return 0;  /* 未填充 */
}

/* 新增：标记寄存器已填充的函数（供异常处理调用） */
void mcd_mark_regset_filled_by_exception(void)
{
    /* 在异常处理中调用此函数标记寄存器已填充 */
    /* 这样 mcd_check_regset_filled 就能检测到 */
}

/* 新增：C 版本的智能 multi_dump */
void mcd_multi_dump_smart(void)
{
    struct thread_info_ops ops;
    
    /* 检查是否已有预填充的寄存器（异常场景） */
    if (!mcd_check_regset_filled())
    {
        printf("DEBUG: No pre-filled registers detected, will use assembly collection\n");
        /* 让汇编代码采集当前寄存器 */
        return;  /* 回到汇编版本处理 */
    }
    else
    {
        printf("DEBUG: Using pre-filled registers from exception handler\n");
    }
    
    /* 生成 CoreDump */
    mcd_rtos_thread_ops(&ops);
    mcd_gen_coredump(&ops);
}



#if defined(__CC_ARM)

#if defined(__ARM_FP) || defined(__VFP_FP__) || defined(__ARMVFP__) || defined(MCD_ARCH_HAS_FPU)

/* clang-format off */
__asm void mcd_mini_dump()
{
    extern get_cur_core_regset_address;
    extern get_cur_fp_regset_address;
    extern mcd_mini_dump_ops;
    extern mcd_gen_coredump;
    extern is_vfp_addressable;

    PRESERVE8

    push {r7, lr}
    sub sp, sp, #24
    add r7, sp, #0

get_regset
    bl get_cur_core_regset_address
    str r0, [r0, #0]
    add r0, r0, #4
    stmia r0!, {r1 - r12}
    mov r1, sp
    add r1, #32
    str r1, [r0, #0]
    ldr r1, [sp, #28]
    str r1, [r0, #4]
    mov r1, pc
    str r1, [r0, #8]
    mrs r1, xpsr
    str r1, [r0, #12]

    bl is_vfp_addressable
    cmp r0, #0
    beq get_reg_done

    bl get_cur_fp_regset_address
    vstmia r0!, {d0 - d15}
    vmrs r1, fpscr
    str r1, [r0, #0]

get_reg_done
    mov r0, r7
    bl mcd_mini_dump_ops
    mov r0, r7
    bl mcd_gen_coredump
    nop
    adds r7, r7, #24
    mov sp, r7
    pop {r7, pc}
    nop
    nop
}

__asm void mcd_multi_dump(void)
{
    extern get_cur_core_regset_address;
    extern get_cur_fp_regset_address;
    extern mcd_rtos_thread_ops;
    extern mcd_gen_coredump;
    extern is_vfp_addressable;

    PRESERVE8

    push {r7, lr}
    sub sp, sp, #24
    add r7, sp, #0

get_regset1
    bl get_cur_core_regset_address
    str r0, [r0, #0]
    add r0, r0, #4
    stmia r0!, {r1 - r12}
    mov r1, sp
    add r1, #32
    str r1, [r0, #0]
    ldr r1, [sp, #28]
    str r1, [r0, #4]
    mov r1, pc
    str r1, [r0, #8]
    mrs r1, xpsr
    str r1, [r0, #12]

    bl is_vfp_addressable
    cmp r0, #0
    beq get_reg_done1

    bl get_cur_fp_regset_address
    vstmia r0!, {d0 - d15}
    vmrs r1, fpscr
    str r1, [r0, #0]

get_reg_done1
    mov r0, r7
    bl mcd_rtos_thread_ops
    mov r0, r7
    bl mcd_gen_coredump
    nop
    adds r7, r7, #24
    mov sp, r7
    pop {r7, pc}
    nop
    nop
}

#else /* no FPU: emit assembly WITHOUT VFP opcodes */

__asm void mcd_mini_dump()
{
    extern get_cur_core_regset_address;
    extern mcd_mini_dump_ops;
    extern mcd_gen_coredump;

    PRESERVE8

    push {r7, lr}
    sub sp, sp, #24
    add r7, sp, #0

get_regset
    bl get_cur_core_regset_address
    mov r1, r0      /* r1 = &current_core_regset */
    
    /* 保存 r0 */
    str r0, [r1], #4
    
    /* 保存 r1-r12，注意避免基址寄存器在存储列表中 */
    stmia r1!, {r2-r12}  /* 从 r2 开始，避免 r1 冲突 */
    
    /* 手动保存 r1（当前是基址寄存器，所以要特殊处理） */
    sub r2, r1, #44     /* r2 指向 r1 的位置 */
    ldr r3, =get_cur_core_regset_address
    str r3, [r2]        /* 这里存储的是函数地址，实际应该存储寄存器值 */
    
    /* 保存 SP (调用前的值) */
    mov r2, sp
    add r2, #32         /* 补偿 push {r7, lr} + sub sp, #24 */
    str r2, [r1], #4
    
    /* 保存 LR */
    ldr r2, [sp, #28]   /* 从栈获取原始 LR */
    str r2, [r1], #4
    
    /* 保存 PC */
    mov r2, pc
    str r2, [r1], #4
    
    /* 保存 xPSR */
    mrs r2, xpsr
    str r2, [r1], #4

get_reg_done
    mov r0, r7
    bl mcd_mini_dump_ops
    mov r0, r7
    bl mcd_gen_coredump
    nop
    adds r7, r7, #24
    mov sp, r7
    pop {r7, pc}
}

__asm void mcd_multi_dump(void)
{
    extern get_cur_core_regset_address;
    extern mcd_check_regset_filled;  /* C 函数：检查是否已填充 */
    extern mcd_multi_dump_smart;     /* C 函数：使用预填充寄存器生成 CoreDump */
    extern mcd_rtos_thread_ops;
    extern mcd_gen_coredump;

    PRESERVE8

    push {r7, lr}
    sub sp, sp, #24
    add r7, sp, #0

    /* 调用 C 函数检查寄存器是否已填充 */
    bl mcd_check_regset_filled
    cmp r0, #1
    beq use_prefilled_regs  /* 如果已填充，使用 C 函数处理 */

    /* 普通调用场景：采集当前寄存器（汇编实现） */
collect_current_regs
    bl get_cur_core_regset_address
    mov r1, r0      /* r1 = &current_core_regset */
    
    /* 先保存一些寄存器到栈上，避免冲突 */
    push {r0-r3}
    
    /* 从栈恢复并保存 r0-r3 */
    pop {r2-r5}     /* r2=原r0, r3=原r1, r4=原r2, r5=原r3 */
    stmia r1!, {r2-r5}  /* 保存原始的 r0-r3 */
    
    /* 保存 r4-r12 */
    stmia r1!, {r4-r12}  /* 现在可以安全保存 */
    
    /* 保存 SP (调用前的值) */
    mov r2, sp
    add r2, #32     /* 补偿 push + sub */
    str r2, [r1], #4
    
    /* 保存 LR */
    ldr r2, [sp, #28]  /* 从栈获取原始 LR */
    str r2, [r1], #4
    
    /* 保存 PC */
    mov r2, pc
    str r2, [r1], #4
    
    /* 保存 xPSR */
    mrs r2, xpsr
    str r2, [r1], #4
    
    /* 继续生成 CoreDump */
    mov r0, r7
    bl mcd_rtos_thread_ops
    mov r0, r7
    bl mcd_gen_coredump
    b cleanup

use_prefilled_regs
    /* 调用 C 函数处理预填充的寄存器 */
    bl mcd_multi_dump_smart

cleanup
    nop
    adds r7, r7, #24
    mov sp, r7
    pop {r7, pc}
}


#endif /* FPU check */

#elif defined(__ARMCC_VERSION) && (__ARMCC_VERSION >= 6010050) || defined(__GNUC__)


#define mcd_get_regset(regset)                                              \
    __asm volatile("  mov r0, %0                \n"                         \
                   "  str r0, [r0 , #0]         \n"                         \
                   "  add r0, r0, #4            \n"                         \
                   "  stmia  r0!, {r1 - r12}    \n"                         \
                   "  mov r1, sp                \n"                         \
                   "  str  r1, [r0, #0]         \n"                         \
                   "  mov r1, lr                \n"                         \
                   "  str  r1, [r0, #4]         \n"                         \
                   "  mov r1, pc                \n"                         \
                   "  str  r1, [r0, #8]         \n"                         \
                   "  mrs r1, xpsr              \n"                         \
                   "  str  r1, [r0, #12]        \n" ::"r"(regset)           \
                   : "memory", "cc");

#define mcd_get_fpregset(regset)                                            \
    __asm volatile(" mov r0, %0                 \n"                         \
                   " vstmia r0!, {d0 - d15}     \n"                         \
                   " vmrs r1, fpscr             \n"                         \
                   " str  r1, [r0, #0]          \n" ::"r"(regset)           \
                   : "memory", "cc");

void mcd_mini_dump(void)
{
    struct thread_info_ops ops;

    mcd_get_regset((uint32_t *)get_cur_core_regset_address());

#if MCD_FPU_SUPPORT
    if (is_vfp_addressable())
        mcd_get_fpregset((uint32_t *)get_cur_fp_regset_address());
#endif

    mcd_mini_dump_ops(&ops);
    mcd_gen_coredump(&ops);
}

void mcd_multi_dump(void)
{
    struct thread_info_ops ops;

    mcd_get_regset((uint32_t *)get_cur_core_regset_address());

#if MCD_FPU_SUPPORT
    if (is_vfp_addressable())
        mcd_get_fpregset((uint32_t *)get_cur_fp_regset_address());
#endif

    mcd_rtos_thread_ops(&ops);
    mcd_gen_coredump(&ops);
}

#endif

/**
 * @brief Collect ARM Cortex-M4 registers from RT-Thread stack frame
 *
 * This function extracts register values from the stack frame created by
 * RT-Thread's context switch mechanism (PendSV_Handler) or exception handling.
 * 
 * RT-Thread Stack Frame Layout (from low to high memory address):
 * +-------------------+  <- stack_top (input parameter)
 * | FPU flag          |  (4 bytes, if MCD_FPU_SUPPORT enabled)
 * +-------------------+
 * | r4                |  (4 bytes, software saved)
 * | r5                |  (4 bytes, software saved)
 * | r6                |  (4 bytes, software saved)
 * | r7                |  (4 bytes, software saved)
 * | r8                |  (4 bytes, software saved)
 * | r9                |  (4 bytes, software saved)
 * | r10               |  (4 bytes, software saved)
 * | r11               |  (4 bytes, software saved)
 * +-------------------+
 * | FPU s16-s31       |  (64 bytes, if FPU context active)
 * +-------------------+
 * | r0                |  (4 bytes, hardware saved)
 * | r1                |  (4 bytes, hardware saved)
 * | r2                |  (4 bytes, hardware saved)
 * | r3                |  (4 bytes, hardware saved)
 * | r12               |  (4 bytes, hardware saved)
 * | lr                |  (4 bytes, hardware saved)
 * | pc                |  (4 bytes, hardware saved)
 * | xpsr              |  (4 bytes, hardware saved)
 * +-------------------+
 * | FPU s0-s15        |  (64 bytes, if FPU context active)
 * | FPSCR             |  (4 bytes, if FPU context active)
 * | NO_NAME           |  (4 bytes, if FPU context active)
 * +-------------------+  <- current SP after context save
 *
 * @param stack_top Pointer to the beginning of the stack frame (FPU flag position)
 * @param core_regset Pointer to structure for storing ARM core registers
 * @param fp_regset Pointer to structure for storing FPU registers
 */
void collect_registers_armv7m(uint32_t *stack_top,
                                    core_regset_type *core_regset,
                                    fp_regset_type *fp_regset)
{    
    /* 
     * This function uses the same stack frame parsing approach as collect_registers_armv7ms
     * for consistency. Both PendSV_Handler and HardFault_Handler now use identical
     * stacking order after the modifications.
     * 
     * Expected stack layout starting from stack_top:
     * [FPU flag] -> [r4-r11] -> [FPU s16-s31] -> [exception frame] -> [FPU s0-s15,FPSCR]
     */
    uint32_t *current_ptr = stack_top;
    
    /* Clear both register sets first to ensure clean state */ 
    mcd_memset(core_regset, 0, sizeof(core_regset_type));
    mcd_memset(fp_regset, 0, sizeof(fp_regset_type));

#if MCD_FPU_SUPPORT
    /* Read FPU flag first - indicates if FPU context was saved */
    uint32_t fpu_flag = *current_ptr++;
#endif

    /* Extract core registers r4-r11 (software saved by RT-Thread) */
    core_regset->r4 = *current_ptr++;
    core_regset->r5 = *current_ptr++;
    core_regset->r6 = *current_ptr++;
    core_regset->r7 = *current_ptr++;
    core_regset->r8 = *current_ptr++;
    core_regset->r9 = *current_ptr++;
    core_regset->r10 = *current_ptr++;
    core_regset->r11 = *current_ptr++;

#if MCD_FPU_SUPPORT
    /* If FPU context is active, s16-s31 registers are saved after r4-r11 */
    if (fpu_flag)
    {
        /* Copy FPU s16-s31 registers (software saved by RT-Thread) */
        for (int i = 16; i < 32; i++)
        {
            ((uint32_t *)fp_regset)[i] = *current_ptr++;
        }
    }
#endif

    /* Extract hardware exception frame (automatically saved by ARM Cortex-M) */
    core_regset->r0 = *current_ptr++;
    core_regset->r1 = *current_ptr++;
    core_regset->r2 = *current_ptr++;
    core_regset->r3 = *current_ptr++;
    core_regset->r12 = *current_ptr++;
    core_regset->lr = *current_ptr++;
    core_regset->pc = *current_ptr++;
    core_regset->xpsr = *current_ptr++;

#if MCD_FPU_SUPPORT
    /* If FPU context is active, s0-s15 and FPSCR are saved after exception frame */
    if (fpu_flag)
    {
        /* Copy FPU s0-s15 registers (hardware saved by ARM Cortex-M) */
        for (int i = 0; i < 16; i++)
        {
            ((uint32_t *)fp_regset)[i] = *current_ptr++;
        }
        
        /* Copy FPSCR register (FPU status and control) */
        fp_regset->fpscr = *current_ptr++;
        
        /* Skip NO_NAME field (reserved/alignment) */
        current_ptr++;
    }
#endif

    /* SP should point to the current stack pointer position after all saved data */
    core_regset->sp = (uintptr_t)current_ptr;
}

/**
 * @brief ARM Cortex-M specific hard fault exception handler for MCoreDump
 * 
 * This function handles ARM Cortex-M specific stack frame processing when a 
 * hard fault occurs. It calculates the proper stack pointer position and
 * extracts register context for coredump generation.
 * 
 * HardFault Stack Frame Layout (created by HardFault_Handler):
 * +-------------------+  <- Exception occurs here
 * | Hardware Exception|  (32 bytes: r0,r1,r2,r3,r12,lr,pc,xpsr)
 * | Stack Frame       |  (+ optional 72 bytes FPU: s0-s15,FPSCR,NO_NAME)
 * +-------------------+  <- context parameter points here
 * | r11               |  (4 bytes, software saved in HardFault_Handler)
 * | r10               |  (4 bytes, software saved in HardFault_Handler)
 * | ...               |  (...)
 * | r4                |  (4 bytes, software saved in HardFault_Handler)
 * +-------------------+
 * | FPU s31           |  (4 bytes, if FPU context active)
 * | FPU s30           |  (4 bytes, if FPU context active)
 * | ...               |  (...)
 * | FPU s16           |  (4 bytes, if FPU context active)
 * +-------------------+
 * | FPU flag          |  (4 bytes, if MCD_FPU_SUPPORT enabled)
 * +-------------------+
 * | EXC_RETURN        |  (4 bytes, contains exception return information)
 * +-------------------+  <- Final stack pointer position
 *
 * @param context Pointer to exception_stack_frame from HardFault_Handler
 * @return int Always returns -1 to indicate fault condition
 */
int armv7m_hard_fault_exception_hook(void *context)
{
    struct exception_stack_frame *exception_stack = (struct exception_stack_frame *)context;
    uint32_t *exc_frame = (uint32_t *)context;
    
    /* 直接填充 core_regset，使用异常发生时的真实寄存器值 */
    core_regset_type *core_regs = get_cur_core_regset_address();
    fp_regset_type *fp_regs = get_cur_fp_regset_address();
    
    /* 清零寄存器结构 */
    mcd_memset(core_regs, 0, sizeof(core_regset_type));
    if (fp_regs) {
        mcd_memset(fp_regs, 0, sizeof(fp_regset_type));
    }
    
    /* 使用异常帧中的寄存器值（这是故障发生时的真实状态）*/
    if (exc_frame)
    {
        core_regs->r0   = exc_frame[0];   /* HardFault 时保存的 r0 */
        core_regs->r1   = exc_frame[1];   /* HardFault 时保存的 r1 */
        core_regs->r2   = exc_frame[2];   /* HardFault 时保存的 r2 */
        core_regs->r3   = exc_frame[3];   /* HardFault 时保存的 r3 */
        core_regs->r12  = exc_frame[4];   /* HardFault 时保存的 r12 */
        core_regs->lr   = exc_frame[5];   /* HardFault 时保存的 lr */
        core_regs->pc   = exc_frame[6];   /* HardFault 时保存的 pc - 故障地址！ */
        core_regs->xpsr = exc_frame[7];   /* HardFault 时保存的 xpsr */
        core_regs->sp   = (uint32_t)exc_frame + 32; /* 异常发生时的 SP */
        
        printf("DEBUG: Exception handler filled registers:\n");
        printf("  PC=0x%08X (fault address)\n", core_regs->pc);
        printf("  LR=0x%08X (return address)\n", core_regs->lr);
        printf("  SP=0x%08X (stack pointer)\n", core_regs->sp);
        
        /* 尝试从栈中恢复 r4-r11（如果可用）*/
        uint32_t *stack_ptr = (uint32_t *)exc_frame;
        stack_ptr -= 8; /* 指向可能的 r4-r11 位置 */
        
        /* 验证栈地址有效性 */
        if ((uint32_t)stack_ptr >= 0x20000000 && (uint32_t)stack_ptr < 0x20020000)
        {
            core_regs->r4  = stack_ptr[0];
            core_regs->r5  = stack_ptr[1];
            core_regs->r6  = stack_ptr[2];
            core_regs->r7  = stack_ptr[3];
            core_regs->r8  = stack_ptr[4];
            core_regs->r9  = stack_ptr[5];
            core_regs->r10 = stack_ptr[6];
            core_regs->r11 = stack_ptr[7];
            printf("  r4-r11 recovered from stack\n");
        }
    }

    printf("Generating CoreDump with fault-time context...\n");

    /* 生成 CoreDump - 现在会使用智能检测 */
    mcd_faultdump(MCD_OUTPUT_SERIAL);

    return 0;
}
