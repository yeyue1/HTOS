/**
 * @file httask.h
 * @brief 任务管理接口与任务控制块定义。
 *
 * 本文件声明任务的创建、删除、延时、优先级、挂起/恢复以及任务通知等接口，
 * 并给出任务控制块（TCB）的布局。TCB 通过三个链表项分别挂在就绪/延时表、
 * 事件表（队列与信号量的等待链表）和全局任务表上。
 */
#ifndef HTTASK_H
#define HTTASK_H

#include "httypes.h"
#include "htlist.h"

/**
 * @brief 启动第一个任务，由汇编实现，切换栈指针后进入任务函数。
 * @note 只在调度器启动时由 htStartScheduler() 调用一次。
 */
void vPortStartFirstTask(void);
/** @brief 启动第一个任务的别名，便于移植层使用统一名字。 */
#define htPortStartFirstTask vPortStartFirstTask

/** @brief 任务入口函数类型，参数为创建任务时传入的 pvParameters。 */
typedef void (*TaskFunction_t)(void *pvParameters);

/** @brief 任务通知的动作类型。 */
typedef enum htNotifyAction {
	HT_NOTIFY_NO_ACTION = 0, /**< 不修改通知值，仅用于唤醒等待任务 */
	HT_NOTIFY_SET_BITS, /**< 按位或写入 ulValue */
	HT_NOTIFY_INCREMENT, /**< 通知值加一，在 UINT32_MAX 处饱和 */
	HT_NOTIFY_SET_VALUE_WITH_OVERWRITE, /**< 直接覆盖通知值 */
	HT_NOTIFY_SET_VALUE_WITHOUT_OVERWRITE, /**< 仅当上一次通知已被取走时才覆盖，否则失败 */
} htNotifyAction_t;

/** @brief 任务通知的内部状态，用于判断"未被取走"的通知能否被覆盖。 */
enum {
	HT_NOTIFY_NOT_WAITING = 0U, /**< 任务当前没有在等待通知 */
	HT_NOTIFY_WAITING, /**< 任务正在等待通知 */
	HT_NOTIFY_RECEIVED, /**< 已收到通知但尚未被取走 */
};

/** @brief 新建任务时填充整个栈的标记值，用于运行时检查栈是否被写穿。 */
#define htTASK_STACK_FILL_VALUE ((StackType_t)0xA5A5A5A5U)

/**
 * @brief 任务控制块（TCB）。
 *
 * 每个任务一个 TCB，内含栈指针、三个链表项、优先级、栈基址、名字与通知状态。
 */
typedef struct htTaskControlBlock {
	/**
	 * @brief 任务栈的当前栈顶。
	 * @warning 必须是结构体的第一个成员（偏移 0），上下文切换按固定偏移保存/恢复
	 *          寄存器组，改变位置会使任务无法正常切换。
	 */
	StackType_t *pxTopOfStack;
	htListItem_t xStateListItem; /**< 状态链表项：挂在就绪表或延时/挂起表中 */
	htListItem_t xEventListItem; /**< 事件链表项：挂在队列或信号量的等待链表里 */
	htListItem_t xAllListItem; /**< 所有任务链表项：挂在内核的任务总表上 */
	UBaseType_t uxPriority; /**< 当前生效的优先级，优先级继承时会临时提高 */
	StackType_t *pxStack; /**< 任务栈的起始地址（栈底），栈溢出检查从这里开始扫描 */
	char pcTaskName[configMAX_TASK_NAME_LEN]; /**< 任务名，长度含结尾的 '\0' */
	UBaseType_t uxTaskState; /**< 任务状态，取值为 htTaskStatus_t 之一 */
	UBaseType_t uxBasePriority; /**< 基础优先级，继承结束后用它恢复 uxPriority */
	uint32_t ulNotifiedValue; /**< 32 位任务通知值 */
	UBaseType_t uxNotificationStatus; /**< 任务通知状态 */
	uint32_t ulRunTimeCounter; /**< 任务运行时间计数，供运行时统计使用 */
	UBaseType_t uxStackDepth; /**< 栈深度，单位为 StackType_t（word） */
	uint8_t ucStaticallyAllocated; /**< 非 0 表示 TCB 与栈由调用方提供，删除时不释放 */
	/* uint32_t ulDelayTime;  原始延时值，仅调试时使用，当前已注释掉 */

} htTCB_t;

/* ------------------------------------------------------------------ */
/* 1. 全局变量                                                          */
/* ------------------------------------------------------------------ */

/** @brief 当前正在运行的任务的 TCB。 */
extern htTCB_t *pxCurrentTCB;
/** @brief 系统中已创建的任务总数。 */
extern UBaseType_t uxCurrentNumberOfTasks;
/** @brief 系统 tick 计数，每个 SysTick 中断加一。 */
extern TickType_t xTickCount;
/** @brief 所有任务的总表，遍历全部任务时使用。 */
extern htList_t pxAllocatedTasksList;
/** @brief 延时任务链表，按唤醒 tick 排序。 */
extern htList_t *pxDelayedTaskList;
/** @brief tick 计数溢出后使用的延时任务链表。 */
extern htList_t *pxOverflowDelayedTaskList;
/** @brief 就绪表数组，按优先级各一张链表。 */
extern htList_t pxReadyTasksLists[];

/* ------------------------------------------------------------------ */
/* 2. 任务创建与删除                                                    */
/* ------------------------------------------------------------------ */

/**
 * @brief 动态创建一个任务。
 *
 * @param[in] pxTaskCode 任务入口函数。
 * @param[in] pcName 任务名，可为 NULL；内部会截断到 configMAX_TASK_NAME_LEN - 1 个字符。
 * @param[in] usStackDepth 栈深度，单位为 word，小于 configMINIMAL_STACK_SIZE 时会被静默抬到最小值。
 * @param[in] pvParameters 传给任务入口的参数。
 * @param[in] uxPriority 初始优先级。
 * @param[out] pxCreatedTask 用于回填新建任务的 TCB 句柄，失败时写 NULL。
 * @return htPASS(0) 表示成功，htFAIL(-1) 表示参数非法或内存不足。
 */
BaseType_t htTaskCreate(TaskFunction_t pxTaskCode, const char *const pcName, const uint16_t usStackDepth,
		void *const pvParameters, UBaseType_t uxPriority, void **const pxCreatedTask);
/**
 * @brief 用调用方提供的栈和控制块静态创建一个任务。
 *
 * @param[in] pxTaskCode 任务入口函数。
 * @param[in] pcName 任务名。
 * @param[in] usStackDepth 栈深度，单位为 word，必须不小于 configMINIMAL_STACK_SIZE。
 * @param[in] pvParameters 传给任务入口的参数。
 * @param[in] uxPriority 初始优先级。
 * @param[in] puxStackBuffer 调用方提供的栈空间。
 * @param[in] pxTaskBuffer 调用方提供的 TCB。
 * @return 成功返回任务句柄，参数非法返回 NULL。
 * @note 仅当 configSUPPORT_STATIC_ALLOCATION 为 1 时才有实际实现。
 */
TaskHandle_t htTaskCreateStatic(TaskFunction_t pxTaskCode, const char *const pcName, uint16_t usStackDepth,
		void *const pvParameters, UBaseType_t uxPriority, StackType_t *puxStackBuffer, htTCB_t *pxTaskBuffer);

/**
 * @brief 删除一个任务并释放其资源。
 *
 * @param[in] xTaskToDelete 待删除任务的句柄。
 * @note 删除自身时不会返回；被删除任务若还持有互斥量，需由调用方保证不留下死锁。
 */
void htTaskDelete(TaskHandle_t xTaskToDelete);

#if (configCHECK_FOR_STACK_OVERFLOW > 0)
/**
 * @brief 检查任务的栈是否被写穿，被写穿时调用 htStackOverflowHook()。
 *
 * @param[in] xTask 待检查的任务，传 NULL 表示当前任务。
 * @note 只检查栈底 4 个 word 是否仍为 htTASK_STACK_FILL_VALUE，这样每次任务切换的额外开销
 *       固定在几个周期之内。仅在 configCHECK_FOR_STACK_OVERFLOW > 0 时参与编译。
 */
void htTaskCheckStackSentinel(TaskHandle_t xTask);
/**
 * @brief 栈溢出钩子，默认实现为死循环停机。
 *
 * @param[in] xTask 发生溢出的任务。
 * @param[in] pcTaskName 发生溢出的任务名。
 * @note 弱符号，应用可自行提供实现来记录故障信息。
 */
void htStackOverflowHook(TaskHandle_t xTask, const char *pcTaskName);
#endif

/* ------------------------------------------------------------------ */
/* 3. 延时与优先级                                                      */
/* ------------------------------------------------------------------ */

/**
 * @brief 让当前任务延时若干个 tick。
 *
 * @param[in] xTicksToDelay 延时的 tick 数；传 0 等价于 htTaskYield()。
 * @note 任务按唤醒时刻插入延时表，随后触发一次调度切走。
 */
void htTaskDelay(TickType_t xTicksToDelay);
/**
 * @brief 让当前任务延时到指定的绝对时刻，用于实现严格周期任务。
 *
 * @param[in,out] pxPreviousWakeTime 上一次的唤醒时刻，函数会把它更新为本次唤醒时刻。
 * @param[in] xTimeIncrement 周期长度，单位为 tick。
 * @return htPASS(0) 表示成功，htFAIL(-1) 表示参数非法或预期的唤醒时刻已经过去。
 * @note 即使返回 htFAIL，*pxPreviousWakeTime 也已被更新，便于下一周期继续对齐。
 */
BaseType_t htTaskDelayUntil(TickType_t *pxPreviousWakeTime, TickType_t xTimeIncrement);
/**
 * @brief 读取任务的优先级。
 *
 * @param[in] xTask 任务句柄，传 NULL 表示当前任务。
 * @return 该任务的当前优先级，句柄无效时返回 0。
 */
UBaseType_t htTaskPriorityGet(TaskHandle_t xTask);
/**
 * @brief 修改任务的优先级。
 *
 * @param[in] xTask 任务句柄，传 NULL 表示当前任务。
 * @param[in] uxNewPriority 新优先级，超出范围会被钳到 configMAX_PRIORITIES - 1。
 * @note 会同步更新基础优先级，并把任务迁移到对应的就绪表；若因此变得可运行会触发一次调度。
 */
void htTaskPrioritySet(TaskHandle_t xTask, UBaseType_t uxNewPriority);

/* ------------------------------------------------------------------ */
/* 4. 挂起与恢复                                                        */
/* ------------------------------------------------------------------ */

/**
 * @brief 挂起任务，被挂起的任务不再参与调度。
 *
 * @param[in] xTaskToSuspend 任务句柄，传 NULL 表示当前任务。
 * @note 对已经挂起的任务重复调用无副作用；挂起自身会立刻让出 CPU。
 */
void htTaskSuspend(TaskHandle_t xTaskToSuspend);
/**
 * @brief 恢复被挂起的任务。
 *
 * @param[in] xTaskToResume 任务句柄。
 * @note 句柄无效或任务本就未挂起时直接忽略；恢复可能引发一次调度。
 */
void htTaskResume(TaskHandle_t xTaskToResume);
/**
 * @brief 中断上下文里恢复被挂起的任务。
 *
 * @param[in] xTaskToResume 任务句柄。
 * @return htTRUE 表示恢复后需要切换任务（被恢复的任务优先级更高），否则返回 htFALSE。
 */
BaseType_t htTaskResumeFromISR(TaskHandle_t xTaskToResume);

/* ------------------------------------------------------------------ */
/* 5. 任务通知                                                          */
/* ------------------------------------------------------------------ */

/**
 * @brief 按指定动作更新目标任务的通知值。
 *
 * @param[in] xTaskToNotify 目标任务句柄。
 * @param[in] ulValue 参与运算的值。
 * @param[in] xAction 通知动作，见 htNotifyAction_t。
 * @return htPASS(0) 表示成功，htFAIL(-1) 表示参数非法或动作条件不满足。
 * @note HT_NOTIFY_SET_VALUE_WITHOUT_OVERWRITE 在目标仍持有未取走的通知时返回 htFAIL；
 *       HT_NOTIFY_INCREMENT 在 UINT32_MAX 处饱和，不再增长。
 */
BaseType_t htTaskNotify(TaskHandle_t xTaskToNotify, uint32_t ulValue, htNotifyAction_t xAction);
/**
 * @brief 中断上下文里按指定动作更新目标任务的通知值。
 *
 * @param[in] xTaskToNotify 目标任务句柄。
 * @param[in] ulValue 参与运算的值。
 * @param[in] xAction 通知动作。
 * @param[out] pxHigherPriorityTaskWoken 若唤醒的任务优先级更高，会被写为 htTRUE。
 * @return htPASS(0) 表示成功，htFAIL(-1) 表示参数非法或动作条件不满足。
 */
BaseType_t htTaskNotifyFromISR(
		TaskHandle_t xTaskToNotify, uint32_t ulValue, htNotifyAction_t xAction, BaseType_t *pxHigherPriorityTaskWoken);
/**
 * @brief 给目标任务发一个轻量信号，等价于通知值加一。
 *
 * @param[in] xTaskToNotify 目标任务句柄。
 * @return htPASS(0) 表示成功，htFAIL(-1) 表示参数非法。
 * @note 等价于 ulValue 为 0 的 HT_NOTIFY_INCREMENT，使用成本低于信号量。
 */
BaseType_t htTaskNotifyGive(TaskHandle_t xTaskToNotify);
/**
 * @brief 中断上下文里给目标任务发一个轻量信号。
 *
 * @param[in] xTaskToNotify 目标任务句柄。
 * @param[out] pxHigherPriorityTaskWoken 若唤醒的任务优先级更高，会被写为 htTRUE。
 * @return htPASS(0) 表示成功，htFAIL(-1) 表示参数非法。
 */
BaseType_t htTaskNotifyGiveFromISR(TaskHandle_t xTaskToNotify, BaseType_t *pxHigherPriorityTaskWoken);
/**
 * @brief 等待任务通知，可带超时。
 *
 * @param[in] ulBitsToClearOnEntry 进入等待前先清零的通知位掩码。
 * @param[in] ulBitsToClearOnExit 取到通知后、返回前清零的通知位掩码。
 * @param[out] pulNotificationValue 用于回填取到的通知值；未取到（超时）时写 0。
 * @param[in] xTicksToWait 最长等待 tick 数，传 htBLOCKED_INDEFINITELY 表示永久等待。
 * @return htPASS(0) 表示取到通知，htFAIL(-1) 表示超时。
 */
BaseType_t htTaskNotifyWait(uint32_t ulBitsToClearOnEntry, uint32_t ulBitsToClearOnExit, uint32_t *pulNotificationValue,
		TickType_t xTicksToWait);
/**
 * @brief 以计数方式等待任务通知，语义类似二值/计数信号量的 take。
 *
 * @param[in] xClearCountOnExit 非 0 表示取走时把通知值清零，为 0 表示只减一。
 * @param[in] xTicksToWait 最长等待 tick 数。
 * @return 取到的通知值；返回 0 表示超时或没有可用通知。
 */
uint32_t htTaskNotifyTake(BaseType_t xClearCountOnExit, TickType_t xTicksToWait);

/* ------------------------------------------------------------------ */
/* 6. 任务查询                                                          */
/* ------------------------------------------------------------------ */

/**
 * @brief 取当前正在运行的任务句柄。
 *
 * @return 当前任务的 TCB，即 pxCurrentTCB。
 */
TaskHandle_t htTaskGetCurrentTaskHandle(void);
/**
 * @brief 读取任务名。
 *
 * @param[in] xTask 任务句柄，传 NULL 表示当前任务。
 * @return 任务名字符串指针；句柄无效返回 NULL。
 */
char *htTaskGetName(TaskHandle_t xTaskToQuery);
/**
 * @brief 读取任务状态。
 *
 * @param[in] xTask 任务句柄。
 * @return 任务状态，取值为 htTaskStatus_t 之一；句柄无效返回 HT_TASK_INVALID。
 */
htTaskStatus_t htTaskGetState(TaskHandle_t xTask);

/* ------------------------------------------------------------------ */
/* 7. 调度与空闲任务                                                    */
/* ------------------------------------------------------------------ */

/**
 * @brief 创建空闲任务，优先级为 0，供调度器在没有就绪任务时运行。
 *
 * @return 创建成功返回 htPASS，失败返回 htFAIL。
 */
void htTaskStartScheduler(void);
/**
 * @brief 递增系统 tick 并处理延时到期任务，由 SysTick 中断调用。
 */
void htTaskTickInc(void);
/**
 * @brief 当前任务主动让出 CPU，触发一次任务切换。
 *
 * @note 通过置位 PendSV 的 PENDSVSET 位实现；主机侧测试时改为调用测试钩子。
 */
void htTaskYield(void);
/**
 * @brief 选出下一个要运行的任务并切换到它。
 *
 * @note 具体规则见 htscheduler.h 中的同名声明说明。
 */
void htTaskSwitchContext(void);
/**
 * @brief 创建空闲任务。
 *
 * @return 成功返回 htPASS，失败返回 htFAIL。
 * @note 堆大小 configTOTAL_HEAP_SIZE 为 0 时改用静态的空闲任务栈与 TCB。
 */
BaseType_t htprvCreateIdleTask(void);

#endif /* HTTASK_H */