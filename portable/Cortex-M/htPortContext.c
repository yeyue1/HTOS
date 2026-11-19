/**
 * @file htPortContext.c
 * @brief Cortex-M3 端口层：naked 上下文切换与硬件异常入口（GCC + Thumb）。
 *
 * 四段汇编分别负责：启动第一个任务（vPortStartFirstTask + SVC_Handler）、
 * 任务间切换（PendSV_Handler）、以及硬 fault 现场取证（HardFault_Handler）。
 * 所有函数都用 naked 属性，保证编译器只发出下面写明的指令，不加序言也不加尾声。
 *
 * 异常进入/返回的约定（Cortex-M3）：
 *   - 异常进入时硬件把 R0-R3、R12、LR、PC、xPSR 共 8 个字压到当时的栈上；
 *   - 剩下的 R4-R11 是编译器约定的被调用者保存寄存器，必须由软件在这里压栈，
 *     这就是本文件里 stmdb/ldmia 那 8 个字的唯一含义；
 *   - EXC_RETURN（LR 的值）第 2 位决定返回后进 Thread 还是 Handler 模式，
 *     第 3 位决定返回后使用 MSP 还是 PSP。
 */

#include "htPort.h"
#include "httask.h"
/* 设备头文件定义 IRQn_Type 与 SCB，并顺带把 CMSIS 内核头文件拉进来。 */
#include HT_PORT_DEVICE_HEADER
#include HT_PORT_CORE_HEADER

#include <stdint.h>

#if !defined(__GNUC__)
#error "portable/Cortex-M/htPortContext.c requires GCC-compatible inline assembly"
#endif

#define HT_PORT_STRINGIFY_IMPL(value) #value
#define HT_PORT_STRINGIFY(value) HT_PORT_STRINGIFY_IMPL(value)

#if HT_PORT_VALIDATE_STACK
#define HT_PORT_STACK_CHECK_R0                                                                                         \
	"ldr r12, =" HT_PORT_STRINGIFY(HT_PORT_SRAM_START) "\n"                                                            \
													   "cmp r0, r12\n"                                                 \
													   "blo PendSV_StackError\n"                                       \
													   "ldr r12, =" HT_PORT_STRINGIFY(                                 \
															   HT_PORT_SRAM_END) "\n"                                  \
																				 "cmp r0, r12\n"                       \
																				 "bhs PendSV_StackError\n"
#endif

/* TCB 的第一个字段是 pxTopOfStack，下面的汇编直接按这个布局取栈顶。 */
extern htTCB_t *pxCurrentTCB;

/**
 * @brief 装载复位时的 MSP，并通过 SVC 进入第一个任务。
 *
 * 第一步 ldr r0,=0xE000ED08 取的是 VTOR（向量表偏移寄存器），
 * 连续两次解引用后得到的就是向量表第 0 项，即上电时写入的初始栈顶 MSP。
 * 把 MSP 复位成它，等于把主栈指针退回到调度器启动前的干净状态；
 * 随后清 BASEPRI、开中断，再执行 svc 0 进入 SVC_Handler。
 *
 * @note 本函数用 naked 属性，GCC 只发出下面这些指令，不添编译器序言与尾声。
 * @warning svc 指令返回后会跳到下方标签 1，即原地死循环：正常路径应当
 *          在 SVC_Handler 里改成任务上下文，永远不回到这里。
 */
__attribute__((naked, used, noinline)) void vPortStartFirstTask(void)
{
	__asm volatile(".syntax unified\n"
				   ".thumb\n"
				   "ldr r0, =0xE000ED08\n"
				   "ldr r0, [r0]\n"
				   "ldr r0, [r0]\n"
				   "msr msp, r0\n"
				   "movs r0, #0\n"
				   "msr basepri, r0\n"
				   "cpsie i\n"
				   "cpsie f\n"
				   "dsb\n"
				   "isb\n"
				   "svc 0\n"
				   "1:\n"
				   "b 1b\n");
}

/**
 * @brief 恢复第一个任务的软件保存寄存器，改用 PSP 返回 Thread 模式。
 *
 * 寄存器分工：r3 指向 &pxCurrentTCB，r1 是取出的 TCB 指针，
 * r0 先取 TCB 第一个字段 pxTopOfStack，再由 ldmia r0!,{r4-r11} 弹出
 * 8 个被调用者保存寄存器并把 r0 顺移 32 字节，随后 msr psp 把它写成任务栈顶。
 *
 * movs r0,#2 加 msr control 是把 CONTROL.SPSEL 置 1，让 Thread 模式使用 PSP；
 * isb 保证 PSP 切换对后续指令立即生效。
 * 最后把 lr 装成 0xFFFFFFFD（返回 Thread 模式、使用 PSP），bx lr 完成异常返回。
 *
 * @note 第一个任务的初始栈帧由 htTaskCreate() 按同样布局准备好，
 *       所以这里弹出来的 R4-R11 与硬件弹出的 8 个字正好拼成完整现场。
 * @note r1 为 0 时跳到标签 1 直接 bx lr，不做任何切换，避免空指针解引用。
 */
__attribute__((naked, used, noinline)) void SVC_Handler(void)
{
	__asm volatile(".syntax unified\n"
				   ".thumb\n"
				   "ldr r3, =pxCurrentTCB\n"
				   "ldr r1, [r3]\n"
				   "cbz r1, 1f\n"
				   "ldr r0, [r1]\n"
				   "ldmia r0!, {r4-r11}\n"
				   "msr psp, r0\n"
				   "movs r0, #2\n"
				   "msr control, r0\n"
				   "isb\n"
				   "ldr lr, =0xFFFFFFFD\n"
				   "bx lr\n"
				   "1:\n"
				   "bx lr\n");
}

/**
 * @brief 保存被打断的任务，用 C 选出下一个任务，再恢复它的软件保存寄存器。
 *
 * 现场保存分工（这是整个移植层最需要对齐的部分）：
 *   - 硬件在异常进入时已经把 R0-R3、R12、LR、PC、xPSR 压到任务栈（PSP）上；
 *   - mrs r0, psp 取到当前任务栈顶，stmdb r0!,{r4-r11} 再把 8 个被调用者
 *     保存寄存器压到同一栈上，r0 随之减 32 字节；
 *   - str r0,[r2] 把新的栈顶写回 TCB 的第一个字段 pxTopOfStack，
 *     下次切回来时 ldr r0,[r1] 就能原样恢复。
 *
 * 调用 C 之前 push {r3,lr}、返回后 pop {r3,lr}：r3 里是 &pxCurrentTCB，
 * lr 里是 EXC_RETURN，两者都必须跨过 htTaskSwitchContext() 调用存活。
 * Handler 模式一律用 MSP，所以这两个字压在 MSP 上，不会污染任务栈。
 *
 * 恢复阶段：r1 是选出的新 TCB，ldmia r0!,{r4-r11} 弹出 8 个字，
 * msr psp 换栈，msr control 置 SPSEL=1，isb 生效。
 *
 * 保存前有两道 cbz 护栏，缺一不可：
 *   - cbz r2,1f 当前没有任务，既没东西可存也没东西可恢复；
 *   - cbz r0,9f PSP 为 0，说明还没有任何任务上下文（第一个任务尚未经
 *     SVC_Handler 进入）；没有这道检查时 stmdb r0! 会写到 0xFFFFFFE0，
 *     内核直接进 HardFault。
 *
 * @warning 标签 9 必须原样返回最初的 EXC_RETURN，绝不能走到下面
 *          orr lr,lr,#0x0D 那段尾巴：那里会强制 Thread 模式改用 PSP，
 *          而 PSP 为 0 恰恰就是标签 9 存在的场景。
 */
__attribute__((naked, used, noinline)) void PendSV_Handler(void)
{
	__asm volatile(".syntax unified\n"
				   ".thumb\n"
				   "cpsid i\n"
				   "mrs r0, psp\n"
				   "ldr r3, =pxCurrentTCB\n"
				   "ldr r2, [r3]\n"
				   "cbz r2, 1f\n"
				   "cbz r0, 9f\n"
#if HT_PORT_VALIDATE_STACK
			HT_PORT_STACK_CHECK_R0
#endif
				   "stmdb r0!, {r4-r11}\n"
				   "str r0, [r2]\n"
				   "1:\n"
				   "push {r3, lr}\n"
				   "bl htTaskSwitchContext\n"
				   "pop {r3, lr}\n"
				   "ldr r1, [r3]\n"
				   "cbz r1, 2f\n"
				   "ldr r0, [r1]\n"
#if HT_PORT_VALIDATE_STACK
			HT_PORT_STACK_CHECK_R0
#endif
				   "ldmia r0!, {r4-r11}\n"
				   "msr psp, r0\n"
				   "movs r0, #2\n"
				   "msr control, r0\n"
				   "isb\n"
				   "2:\n"
				   "cpsie i\n"
				   "movs r0, #0x0D\n"
				   "orr lr, lr, r0\n"
				   "bx lr\n"
				   "9:\n"
				   "cpsie i\n"
				   "bx lr\n"
#if HT_PORT_VALIDATE_STACK
				   "PendSV_StackError:\n"
				   "udf #0\n"
				   "b .\n"
#endif
	);
}

/* 可选的架构钩子，由 Trace/coredump 集成方提供实现，这里是弱声明。 */
extern int armv7m_hard_fault_exception_hook(void *context) __attribute__((weak));

#ifndef SCB_CFSR_NOCP_Msk
#define SCB_CFSR_NOCP_Msk 0U
#endif
#ifndef SCB_CFSR_INVPC_Msk
#define SCB_CFSR_INVPC_Msk 0U
#endif
#ifndef SCB_CFSR_INVSTATE_Msk
#define SCB_CFSR_INVSTATE_Msk 0U
#endif
#ifndef SCB_CFSR_UNDEFINSTR_Msk
#define SCB_CFSR_UNDEFINSTR_Msk 0U
#endif

/**
 * @brief 硬 fault 现场输出钩子的空默认实现。
 *
 * @param[in] pxFrame 现场快照，此处不使用。
 *
 * @note 应用层给出同名强符号即可接管输出，见 htPort.h 的说明。
 */
__attribute__((weak)) void htFaultReport(const htFaultFrame_t *pxFrame)
{
	(void)pxFrame;
}

/**
 * @brief 硬 fault 处理器的 C 部分：收集现场并交给 htFaultReport()。
 *
 * @param[in] fault_args 指向异常栈帧的指针，由 HardFault_Handler 按
 *                       EXC_RETURN 选择 MSP 或 PSP 后传入。
 *
 * fault_args[0..7] 依次是硬件压栈的 R0、R1、R2、R3、R12、LR、PC、xPSR，
 * 随后再读 SCB 的 CFSR/HFSR/DFSR/AFSR/BFAR/MMFAR 补齐状态寄存器。
 *
 * @note 这里故意不依赖板级 HAL/GPIO，同一份移植层配任意 CMSIS 设备头文件都能编译；
 *       需要落盘 coredump 时由工程实现上面那个弱钩子即可。
 * @warning 本函数在异常上下文里运行且不会返回：进函数先 __disable_irq()，
 *          结尾是死循环，因此现场只能原样交给 htFaultReport() 输出，
 *          不能在这里做延时、调度或任何内存申请。
 */
void HardFault_Handler_C(uint32_t *fault_args)
{
	__disable_irq();
	if (fault_args == NULL) {
		for (;;) {
		}
	}

	htFaultFrame_t xFrame;
	uint32_t uxIndex;

	xFrame.ulStackPointer = (uint32_t)(uintptr_t)fault_args;
	for (uxIndex = 0U; uxIndex < 8U; uxIndex++) {
		xFrame.ulCore[uxIndex] = fault_args[uxIndex];
	}

	xFrame.ulCfsr = SCB->CFSR;
	xFrame.ulHfsr = SCB->HFSR;
	xFrame.ulDfsr = SCB->DFSR;
	xFrame.ulAfsr = SCB->AFSR;
	xFrame.ulBfar = SCB->BFAR;
	xFrame.ulMmfar = SCB->MMFAR;
	xFrame.ulFaultFlags = 0U;

	if ((xFrame.ulCfsr & SCB_CFSR_NOCP_Msk) != 0U) {
		xFrame.ulFaultFlags |= HT_FAULT_FLAG_NOCP;
	}
	if ((xFrame.ulCfsr & SCB_CFSR_INVPC_Msk) != 0U) {
		xFrame.ulFaultFlags |= HT_FAULT_FLAG_INVPC;
	}
	if ((xFrame.ulCfsr & SCB_CFSR_INVSTATE_Msk) != 0U) {
		xFrame.ulFaultFlags |= HT_FAULT_FLAG_INVSTATE;
	}
	if ((xFrame.ulCfsr & SCB_CFSR_UNDEFINSTR_Msk) != 0U) {
		xFrame.ulFaultFlags |= HT_FAULT_FLAG_UNDEFINSTR;
	}

	if (armv7m_hard_fault_exception_hook != NULL) {
		(void)armv7m_hard_fault_exception_hook(fault_args);
	}

	htFaultReport(&xFrame);

	for (;;) {
	}
}

/**
 * @brief 依据 EXC_RETURN 选择 MSP 或 PSP，再尾调用 C 报告函数。
 *
 * tst lr,#4 检测 EXC_RETURN 第 2 位：为 1 表示异常发生在 Thread 模式，
 * 现场在 PSP 上，取 psp；为 0 表示发生在 Handler 模式，现场在 MSP 上，取 msp。
 * 取到的栈帧放进 r0，然后 ldr r1,=HardFault_Handler_C 并 bx r1 直接跳过去，
 * 这样 C 函数看到的第一个参数就是正确的栈帧指针。
 *
 * @note 用 naked 属性，避免编译器在本函数里压栈而破坏 r0 的传参。
 */
__attribute__((naked, used, noinline)) void HardFault_Handler(void)
{
	__asm volatile(".syntax unified\n"
				   ".thumb\n"
				   "tst lr, #4\n"
				   "ite eq\n"
				   "mrseq r0, msp\n"
				   "mrsne r0, psp\n"
				   "ldr r1, =HardFault_Handler_C\n"
				   "bx r1\n");
}
