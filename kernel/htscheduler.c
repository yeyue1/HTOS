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
#include "htPort.h"
#include "httrace.h"
#include "htschedpolicy.h"
#include "htlatency.h"
#include "htkernel_internal.h"
#if (configUSE_TIMERS == 1)
#include "httimer.h"
#endif
#include <stddef.h> /* 提供 NULL 定义 */

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
	xNextTaskUnblockTime = (TickType_t)htBLOCKED_INDEFINITELY;

	/* 创建空闲任务；失败则放弃启动。 */
	if (htprvCreateIdleTask() != htPASS) {
		htTrace("FATAL: Failed to create idle task!\r\n");
		return;
	}

#if (configUSE_TIMERS == 1)
	/* 启用软件定时器时自动创建守护任务。 */
	if (htTimerCreateDaemonTask() != htPASS) {
		htTrace("FATAL: Failed to create timer daemon task!\r\n");
		return;
	}
#endif

	/*
	 * 关中断状态下完成优先级配置与首任务选优，杜绝 SysTick 在首个任务尚未
	 * 经 SVC 建立 PSP 上下文前打断 htTaskSwitchContext() 或提前触发 PendSV。
	 */
	htPortDisableAllInterrupts();
	uxCriticalNesting = 0U;
	uxSchedulerSuspended = 0U;
	xYieldPending = htFALSE;

	htPortSetupInterruptPriorities();

	/* 更新系统状态为运行中并选出真正的最高优先级首个任务。 */
	xSchedulerState = HT_SCHEDULER_RUNNING;
	htTaskSwitchContext();

	/* 启动第一个任务：重置 MSP、清 BASEPRI、开中断并经 SVC 进入线程模式，正常不返回。 */
	vPortStartFirstTask();
}

/**
 * @brief 挂起调度器（支持嵌套计数）：期间滴答与让出只登记待补请求而不切换。
 */
void htSuspendScheduler(void)
{
	htEnterCritical();
	if (xSchedulerState != HT_SCHEDULER_NOT_STARTED) {
		uxSchedulerSuspended++;
		xSchedulerState = HT_SCHEDULER_SUSPENDED;
	}
	htExitCritical();
}

/**
 * @brief 恢复调度器（嵌套计数归零时真正恢复，并补发期间推迟的切换请求）。
 */
void htResumeScheduler(void)
{
	BaseType_t xShouldYield = htFALSE;

	htEnterCritical();
	if (uxSchedulerSuspended > 0U) {
		uxSchedulerSuspended--;
		if (uxSchedulerSuspended == 0U) {
			if (xSchedulerState == HT_SCHEDULER_SUSPENDED) {
				xSchedulerState = HT_SCHEDULER_RUNNING;
			}
			if (xYieldPending == htTRUE) {
				xYieldPending = htFALSE;
				xShouldYield = htTRUE;
			}
		}
	}
	htExitCritical();

	if (xShouldYield == htTRUE) {
		htTaskYield();
	}
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
	 * 必须先屏蔽中断、再增加嵌套计数，否则在增量后屏蔽前的窗口里一旦发生抢占，
	 * 换出的上下文会多带一层嵌套，导致退出时嵌套计数永远无法归零。
	 */
	htPortDisableInterrupts();
	uxCriticalNesting++;

	/*
	 * 打点放在计数之后：仪表只统计最外层（nesting == 1）的那一段。
	 */
	HT_LATENCY_CRITICAL_ENTER();
}

void htExitCritical(void)
{
	/* 防御：计数为 0 时直接返回，避免无符号下溢后永久屏蔽中断。 */
	if (uxCriticalNesting > 0U) {
		/*
		 * 结算必须在递减之前：仪表靠 "nesting == 1" 认定最外层出口，
		 * 也要赶在恢复中断之前算完。
		 */
		HT_LATENCY_CRITICAL_EXIT();

		uxCriticalNesting--;

		/* 只有最外层退出才解除中断屏蔽。 */
		if (uxCriticalNesting == 0U) {
			htPortEnableInterrupts();
		}
	}
}

UBaseType_t htEnterCriticalFromISR(void)
{
	return (UBaseType_t)htPortEnterCriticalFromISR();
}

void htExitCriticalFromISR(UBaseType_t uxSavedInterruptStatus)
{
	htPortExitCriticalFromISR((uint32_t)uxSavedInterruptStatus);
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
	/* 调度器被挂起或未运行：记录待补切换请求后返回。 */
	if (uxSchedulerSuspended != 0U || xSchedulerState != HT_SCHEDULER_RUNNING) {
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
 * @brief 判断是否需要请求一次上下文切换。
 *
 * @return 需要切换返回 htTRUE，否则返回 htFALSE。
 *
 * @note 调度器未运行或处于挂起状态时永远返回 htFALSE（挂起期间登记 xYieldPending）。
 */
BaseType_t htSchedulerNeedsSwitch(void)
{
	if (xSchedulerState != HT_SCHEDULER_RUNNING || uxSchedulerSuspended != 0U) {
		if (uxSchedulerSuspended != 0U && pxCurrentTCB != NULL &&
				htSchedGetPolicy()->xShouldPreempt(pxCurrentTCB) == htTRUE) {
			xYieldPending = htTRUE;
		}
		return htFALSE;
	}

	return htSchedGetPolicy()->xShouldPreempt(pxCurrentTCB);
}

/**
 * @brief 调度器是否已经启动过。
 */
BaseType_t htSchedulerIsStarted(void)
{
	return (xSchedulerState == HT_SCHEDULER_NOT_STARTED) ? htFALSE : htTRUE;
}
