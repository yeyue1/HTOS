/**
 * @file htscheduler.c
 * @brief 调度器状态机、临界区、任务选择与上下文切换入口。
 *
 * 本文件只做 C 层决策：选哪个任务、什么时候置位 PendSV。寄存器现场的
 * 保存与恢复在 portable/Cortex-M/htPortContext.c 的 SVC_Handler /
 * PendSV_Handler 汇编里完成。PendSV 被配置为最低优先级，所以切换总在
 * 所有中断退出后执行，不会打断中断服务程序；首次进入任务走的是 svc 0。
 */
#include "htscheduler.h"
#include "httask.h"
#include "htlist.h"
#include "stm32f1xx_hal.h"
#include "httrace.h"
#include "htschedpolicy.h"
#include <stddef.h> /* 提供 NULL 定义 */
#include "core_cm3.h" /* 提供 NVIC 与 SCB 定义 */

/* ------------------------------------------------------------------ */
/* 外部函数与变量声明                                                 */
/* ------------------------------------------------------------------ */
extern void prvInitialiseTaskLists(void);
extern void htTaskYield(void);

/** 当前运行的任务控制块；PendSV 保存现场时把 SP 写入它指向的 TCB。 */
extern htTCB_t *pxCurrentTCB;
/** 各优先级的就绪链表，下标即优先级，数值越大优先级越高。 */
extern htList_t pxReadyTasksLists[];
/** 两条延时链表交替使用：一条放本轮到期的任务，一条放回绕后的任务。 */
extern htList_t xDelayedTaskList1;
extern htList_t xDelayedTaskList2;
/** 当前生效的延时链表与溢出链表，tick 回绕时两者交换。 */
extern htList_t *pxDelayedTaskList;
extern htList_t *pxOverflowDelayedTaskList;
/** 系统滴答计数，单位 tick。 */
extern TickType_t xTickCount;

/* ------------------------------------------------------------------ */
/* 调度器私有状态                                                     */
/* ------------------------------------------------------------------ */
/** 调度器状态：未启动 / 运行中 / 已挂起。 */
static htSchedulerState_t xSchedulerState = HT_SCHEDULER_NOT_STARTED;
/** 调度器挂起嵌套计数，0 表示未挂起。 */
static UBaseType_t uxSchedulerSuspended = 0;
/** 挂起期间被推迟的切换请求，恢复调度时补一次让出。 */
static BaseType_t xYieldPending = htFALSE;

/**
 * 临界区嵌套计数器。
 *
 * 每层 htEnterCritical() 加一、htExitCritical() 减一，只有减到 0 才重新
 * 开中断。该变量会被中断与调度代码访问，必须保持 volatile。
 */
volatile UBaseType_t uxCriticalNesting = 0U;

/**
 * @brief 端口层启动入口：清零临界区计数后启动第一个任务。
 *
 * @return 正常路径不会返回；返回即表示启动失败，值为 htFAIL。
 */
BaseType_t htPortStartScheduler(void)
{
	/* 重置临界区嵌套计数器，保证第一次 htExitCritical() 就能开中断。 */
	uxCriticalNesting = 0;

	/* 启动第一个任务：naked 汇编重置 MSP，再以 svc 0 进入 SVC_Handler。 */
	htPortStartFirstTask();

	/* 走到这里说明启动失败。 */
	return htFAIL;
}

/**
 * @brief 初始化调度器：清空任务链表与全部调度状态。
 *
 * @note 必须在创建任何任务之前调用；重复调用会重新清空就绪/延时链表。
 */
void htSchedulerInit(void)
{
	/* 初始化各优先级就绪链表、延时链表、挂起链表与终止链表。 */
	prvInitialiseTaskLists();

	/* 复位调度器状态、挂起计数、推迟的切换请求与临界区计数。 */
	xSchedulerState = HT_SCHEDULER_NOT_STARTED;
	uxSchedulerSuspended = 0;
	xYieldPending = htFALSE;
	uxCriticalNesting = 0;
}

/**
 * @brief 启动调度器：创建空闲任务、配置中断优先级并切入第一个任务。
 *
 * @warning 必须已创建至少一个任务，否则只打印错误后直接返回。
 * @note 最后一步 vPortStartFirstTask() 不会返回。
 */
void htStartScheduler(void)
{
	/* 关键安全检查：一个任务都没有时放弃启动。 */
	if (pxCurrentTCB == NULL) {
		htTrace("FATAL: No tasks available to run!\r\n");
		return;
	}

	/* 确保延时链表可用，并让当前/溢出指针各指向一条链表。 */
	htListInit(&xDelayedTaskList1);
	htListInit(&xDelayedTaskList2);
	pxDelayedTaskList = &xDelayedTaskList1;
	pxOverflowDelayedTaskList = &xDelayedTaskList2;

	/* 创建空闲任务；失败则放弃启动。 */
	if (htprvCreateIdleTask() != htPASS) {
		htTrace("FATAL: Failed to create idle task!\r\n");
		return;
	}

	/* 配置中断优先级：PendSV 取最低（0xFF），SysTick 取较高（0x70）。 */
	NVIC_SetPriority(PendSV_IRQn, 0xFF); /* 最低优先级：等所有中断退出后才切换。 */
	NVIC_SetPriority(SysTick_IRQn, 0x70); /* 高于 PendSV，保证滴答按时到来。 */

	/* 打开中断，再用 DSB/ISB 确保配置立即生效。 */
	__enable_irq();
	__DSB();
	__ISB();

	/* 更新系统状态为运行中。 */
	xSchedulerState = HT_SCHEDULER_RUNNING;

	/* 必须先按优先级选出真正的第一个任务：htTaskCreate() 只在创建第一个任务时
	   顺便把 pxCurrentTCB 赋成它，之后创建的高优先级任务不会回填。若此时直接启动，
	   实际运行的是"最先创建的任务"，却与 PendSV 里保存 SP 用的 pxCurrentTCB
	   不是同一个对象，会把这个任务的栈指针写进另一个 TCB，造成栈交叉污染
	   （实板现象：HardFault + CFSR=INVPC）。 */
	htTaskSwitchContext();

	/* 启动第一个任务：经 SVC 进入线程模式，正常不返回。 */
	vPortStartFirstTask();
}

/**
 * @brief 挂起调度器：把状态从运行中改为已挂起。
 *
 * @note 只改状态标志、不动当前任务；未启动或已挂起时调用无副作用。
 */
void htSuspendScheduler(void)
{
	/* 进入临界区，避免状态切换被中断打断。 */
	htEnterCritical();

	/* 只有运行中才转为已挂起。 */
	if (xSchedulerState == HT_SCHEDULER_RUNNING) {
		xSchedulerState = HT_SCHEDULER_SUSPENDED;
	}

	/* 退出临界区。 */
	htExitCritical();
}

/**
 * @brief 恢复调度器：把状态从已挂起改回运行中。
 *
 * @note 这里只恢复状态、不补发被推迟的切换；补发由成对计数接口
 *       htSchedulerSuspend()/htSchedulerResume() 在计数归零时完成。
 */
void htResumeScheduler(void)
{
	/* 进入临界区。 */
	htEnterCritical();

	/* 仅当处于已挂起状态时才恢复。 */
	if (xSchedulerState == HT_SCHEDULER_SUSPENDED) {
		xSchedulerState = HT_SCHEDULER_RUNNING;

		/* 注意：这里没有补发切换，实际实现可能在恢复时检查一次是否需要调度。 */
	}

	/* 退出临界区。 */
	htExitCritical();
}

/**
 * @brief 系统滴答处理函数，由 SysTick 中断调用。
 *
 * @note 只做 tick 递增与延时到期检查；切换请求由 htTaskTickInc() 内部置位。
 */
void htSchedulerTickHandler(void)
{
	htTaskTickInc();
}

/**
 * @brief 进入内核临界区（可嵌套）。
 *
 * @note 每层调用关一次中断并累加计数，必须与 htExitCritical() 成对使用。
 */
void htEnterCritical(void)
{
	/*
	 * 必须先关中断、再增加嵌套计数。
	 *
	 * 反过来的顺序会留下一个可抢占窗口：计数已经加一，但 PRIMASK 仍为 0。
	 * 若 SysTick/PendSV 恰好落在这一瞬间，上下文会带着"多出来的一层嵌套"
	 * 切走；此后该任务 htExitCritical() 只把计数减到 1，永远走不到
	 * cpsie i，PRIMASK 就永久停在 1，整个系统死锁。
	 *
	 * 压测复现：7 个同优先级任务 + 时间片轮转（每 tick 置位 PendSV）下，
	 * 默认配置必然在数秒内死锁；把顺序改为先 cpsid 后计数即消失。
	 */
	__asm volatile("cpsid i");
	uxCriticalNesting++;
}

/**
 * @brief 退出内核临界区：嵌套计数减到 0 时重新开中断。
 *
 * @note 在 Cortex-M 上据此决定是否执行 cpsie i；计数为 0 时调用不会开中断。
 */
void htExitCritical(void)
{
	/* 防御：计数为 0 时直接返回，避免无符号下溢后永久关中断。 */
	if (uxCriticalNesting > 0) {
		uxCriticalNesting--;

		/* 只有最外层退出才执行 cpsie i。 */
		if (uxCriticalNesting == 0) {
			__asm volatile("cpsie i");
		}
	}
}

/**
 * @brief 选出下一个要运行的任务并更新 pxCurrentTCB。
 *
 * @note 由 PendSV_Handler 在保存完现场后调用；调度器被挂起时只置位
 *       xYieldPending 并立即返回。
 * @note 同一优先级内靠推进 pxList->pxIndex 实现时间片轮转。
 */
void htTaskSwitchContext(void)
{
	/* 调度器被挂起：记录一次待补的切换请求后返回。 */
	if (uxSchedulerSuspended != 0) {
		/* 调度器被挂起，设置挂起标志 */
		xYieldPending = htTRUE;
		return;
	}

	/* 正常走一次选择，清掉待补标志。 */
	xYieldPending = htFALSE;

	/* 当前 TCB 无效时不做选择，现场交给 PendSV 的守卫处理。 */
	if (pxCurrentTCB == NULL) {
		return;
	}

#if (configCHECK_FOR_STACK_OVERFLOW > 0)
	/* 换下之前先确认它没有写崩自己的栈 */
	htTaskCheckStackSentinel(pxCurrentTCB);
#endif

	/* 具体选谁由当前策略决定：内核只认策略接口，不认识任何具体算法。 */
	htTCB_t *pxNewTCB = htSchedGetPolicy()->pxSelectNext();
	if (pxNewTCB != NULL) {
		/* 标记为运行态并把它设为当前任务。 */
		pxNewTCB->uxTaskState = HT_TASK_RUNNING;
		pxCurrentTCB = pxNewTCB;
	}
}

/**
 * @brief 挂起调度器（嵌套计数版），供内核内部成对调用。
 *
 * @return 挂起后的嵌套计数。
 *
 * @note 与 htSchedulerResume() 配对；计数非 0 期间 htTaskSwitchContext()
 *       只置位 xYieldPending，不会真正切换。
 */
UBaseType_t htSchedulerSuspend(void)
{
	/* 增加挂起计数并返回新值。 */
	uxSchedulerSuspended++;

	return uxSchedulerSuspended;
}

/**
 * @brief 恢复调度器（嵌套计数版）：计数归零且有待补请求时补一次让出。
 */
void htSchedulerResume(void)
{
	/* 只在确实挂起过时递减，避免无符号下溢。 */
	if (uxSchedulerSuspended > 0) {
		/* 递减嵌套计数。 */
		uxSchedulerSuspended--;

		/* 完全恢复且存在待补的切换请求时，立即补一次让出。 */
		if (uxSchedulerSuspended == 0 && xYieldPending == htTRUE) {
			/* 触发上下文切换 */
			htTaskYield();
		}
	}
}

/**
 * @brief 判断是否需要请求一次上下文切换。
 *
 * @return 需要切换返回 htTRUE，否则返回 htFALSE。
 *
 * @note 调度器未运行时永远返回 htFALSE；是否响应滴答取决于
 *       configUSE_PREEMPTION 与 configUSE_TIME_SLICING。
 */
BaseType_t htSchedulerNeedsSwitch(void)
{
	/*
	 * 调度器尚未运行之前绝不请求上下文切换。
	 *
	 * board_clock_init() 里的 HAL_Init() 一开机就使能了 SysTick，而第一个任务
	 * 创建完成后 pxCurrentTCB 已经非空，二者叠加会在"任务已建好、第一个任务
	 * 上下文还没建立"之间开出一个窗口：此时若置位 PendSV，PendSV 会在
	 * PSP 仍为 0 的情况下执行保存动作，把 r4-r11 写到 0xFFFFFFE0，直接进
	 * HardFault。滴答计数照常累加，只是暂不请求切换。
	 */
	if (xSchedulerState != HT_SCHEDULER_RUNNING) {
		return htFALSE;
	}

	/*
	 * 是否切换、什么时候切换，全部交给当前策略判断：固定优先级策略看"有更高
	 * 优先级就绪"与"同优先级有兄弟"，公平分享策略看虚拟时间，最早死期策略看
	 * 死期。内核在这里只做"调度器是否在运行"这一层通用守卫。
	 */
	return htSchedGetPolicy()->xShouldPreempt(pxCurrentTCB);
}

/**
 * @brief 调度器是否已经启动过。
 */
BaseType_t htSchedulerIsStarted(void)
{
	return (xSchedulerState == HT_SCHEDULER_NOT_STARTED) ? htFALSE : htTRUE;
}
