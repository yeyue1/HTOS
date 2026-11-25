/**
 * @file httimer.h
 * @brief 软件定时器接口（受 configUSE_TIMERS 控制，默认关闭零开销）。
 *
 * 提供单次（one-shot）与周期自动重载（auto-reload）软件定时器，支持动态与静态创建、
 * 任务与 ISR 上下文控制命令，并在守护任务上下文执行回调（不占用 SysTick 中断时间）。
 */
#ifndef HT_TIMER_H
#define HT_TIMER_H

#include "httypes.h"
#include "htlist.h"

/** @brief 软件定时器句柄。 */
typedef void *TimerHandle_t;

/** @brief 软件定时器到期回调函数原型（在守护任务上下文中调用）。 */
typedef void (*htTimerCallback_t)(TimerHandle_t xTimer);

/**
 * @brief 软件定时器控制块，公开布局供 htTimerCreateStatic() 静态分配使用。
 */
typedef struct htTimerControlBlock {
	htListItem_t xTimerListItem; /**< 挂在活动定时器链表上，xItemValue 存到期绝对 tick */
	TickType_t xTimerPeriodInTicks; /**< 定时周期（tick，必须大于 0） */
	htTimerCallback_t pxCallback; /**< 到期回调函数 */
	void *pvTimerID; /**< 调用方自定义上下文 ID */
	char pcTimerName[configMAX_TASK_NAME_LEN]; /**< 定时器名称 */
	uint8_t ucAutoReload; /**< 非 0 表示周期自动重载，0 表示单次触发 */
	uint8_t ucActive; /**< 非 0 表示当前正在活动链表中计时 */
	uint8_t ucStaticallyAllocated; /**< 非 0 表示静态存储，删除时不释放内存 */
} htTimer_t;

#if (configUSE_TIMERS == 1)

/**
 * @brief 初始化软件定时器服务内部链表与命令队列（幂等）。
 *
 * @return 初始化成功返回 htPASS，失败返回 htFAIL。
 */
BaseType_t htTimerInit(void);

/**
 * @brief 创建软件定时器守护任务（由 htStartScheduler() 自动调用，幂等）。
 *
 * @return 成功返回 htPASS，失败返回 htFAIL。
 */
BaseType_t htTimerCreateDaemonTask(void);

/**
 * @brief 动态创建一个软件定时器（初始为停止态，需调用 htTimerStart() 启动）。
 *
 * @param[in] pcTimerName 定时器名称，可为 NULL。
 * @param[in] xTimerPeriodInTicks 定时周期（tick），须在 1..INT32_MAX 之间。
 * @param[in] xAutoReload htTRUE 为周期自动重载，htFALSE 为单次触发。
 * @param[in] pvTimerID 用户自定义上下文指针。
 * @param[in] pxCallbackFunction 到期回调函数，不可为 NULL。
 * @return 成功返回定时器句柄，参数非法或内存不足返回 NULL。
 */
TimerHandle_t htTimerCreate(const char *pcTimerName, TickType_t xTimerPeriodInTicks, BaseType_t xAutoReload,
		void *pvTimerID, htTimerCallback_t pxCallbackFunction);

/**
 * @brief 使用调用方提供的控制块静态创建一个软件定时器（不申请堆）。
 */
TimerHandle_t htTimerCreateStatic(const char *pcTimerName, TickType_t xTimerPeriodInTicks, BaseType_t xAutoReload,
		void *pvTimerID, htTimerCallback_t pxCallbackFunction, htTimer_t *pxTimerBuffer);

/** @brief 启动（或重新启动）定时器。 */
BaseType_t htTimerStart(TimerHandle_t xTimer, TickType_t xTicksToWait);
/** @brief 中断上下文启动定时器。 */
BaseType_t htTimerStartFromISR(TimerHandle_t xTimer, BaseType_t *pxHigherPriorityTaskWoken);

/** @brief 停止定时器（从活动链表摘除）。 */
BaseType_t htTimerStop(TimerHandle_t xTimer, TickType_t xTicksToWait);
/** @brief 中断上下文停止定时器。 */
BaseType_t htTimerStopFromISR(TimerHandle_t xTimer, BaseType_t *pxHigherPriorityTaskWoken);

/** @brief 复位定时器，从当前时刻起重新计算到期时间。 */
BaseType_t htTimerReset(TimerHandle_t xTimer, TickType_t xTicksToWait);
/** @brief 中断上下文复位定时器。 */
BaseType_t htTimerResetFromISR(TimerHandle_t xTimer, BaseType_t *pxHigherPriorityTaskWoken);

/** @brief 修改定时器周期并立即重新启动计时。 */
BaseType_t htTimerChangePeriod(TimerHandle_t xTimer, TickType_t xNewPeriod, TickType_t xTicksToWait);
/** @brief 中断上下文修改定时器周期。 */
BaseType_t htTimerChangePeriodFromISR(
		TimerHandle_t xTimer, TickType_t xNewPeriod, BaseType_t *pxHigherPriorityTaskWoken);

/** @brief 删除定时器；动态创建的定时器将在守护任务处理该命令时释放。 */
BaseType_t htTimerDelete(TimerHandle_t xTimer, TickType_t xTicksToWait);

/** @brief 查询定时器当前是否处于活动计时状态。 */
BaseType_t htTimerIsActive(TimerHandle_t xTimer);
/** @brief 读取定时器当前周期（tick）。 */
TickType_t htTimerGetPeriod(TimerHandle_t xTimer);
/** @brief 读取定时器用户上下文 ID。 */
void *htTimerGetTimerID(TimerHandle_t xTimer);
/** @brief 修改定时器用户上下文 ID。 */
void htTimerSetTimerID(TimerHandle_t xTimer, void *pvNewID);
/** @brief 读取定时器名称字符串。 */
const char *htTimerGetName(TimerHandle_t xTimer);
/** @brief 获取软件定时器守护任务句柄。 */
TaskHandle_t htTimerGetDaemonTaskHandle(void);

/**
 * @brief 处理当前已入队的全部定时器命令并触发所有到期回调，返回距下一次到期的等待 tick 数。
 *
 * @param[in] xNow 当前系统 tick 时刻。
 * @return 下一次需要唤醒的等待 tick 数；无活动定时器时返回 htBLOCKED_INDEFINITELY。
 * @note 守护任务主循环调用本函数；主机单元测试可直接调用以确定性步进定时器状态机。
 */
TickType_t htTimerProcessStep(TickType_t xNow);

#endif /* configUSE_TIMERS == 1 */

#endif /* HT_TIMER_H */
