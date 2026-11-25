/**
 * @file httypes.h
 * @brief htOS 的基本类型、常量、枚举与链表结构定义。
 *
 * 本文件不依赖其它内核模块，只引入标准整数类型与编译配置，
 * 供 htlist.h / httask.h / htqueue.h 等头文件共同使用。
 */
#ifndef HT_TYPES_H
#define HT_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include "htconfig.h"

/* ------------------------------------------------------------------ */
/* 1. 基本类型                                                          */
/* ------------------------------------------------------------------ */

/** @brief 布尔与返回值类型，int 宽度，保证与 C 语言的 int 语义一致。 */
typedef int BaseType_t;
/** @brief 无符号计数值类型，用于计数、长度与优先级。 */
typedef unsigned int UBaseType_t;
/** @brief 系统节拍计数类型，单位 tick，32 位会自然回绕。 */
typedef uint32_t TickType_t;
/** @brief 任务句柄，实际指向任务的 TCB。 */
typedef void *TaskHandle_t;
/** @brief 栈单元类型，栈深度以该类型的个数计。 */
typedef uint32_t StackType_t;

/* ------------------------------------------------------------------ */
/* 2. 常量                                                              */
/* ------------------------------------------------------------------ */

/** @brief 函数执行成功。 */
#define htPASS 0
/** @brief 函数执行失败（超时、参数非法或资源不足）。 */
#define htFAIL -1
/** @brief 布尔真。 */
#define htTRUE 1
/** @brief 布尔假。 */
#define htFALSE 0
/** @brief 空指针常量。 */
#define htNULL NULL

/* ------------------------------------------------------------------ */
/* 3. 任务与调度器状态                                                  */
/* ------------------------------------------------------------------ */

/**
 * @brief 任务状态。
 * - HT_TASK_READY：在就绪表中，等待 CPU；
 * - HT_TASK_RUNNING：正在运行；
 * - HT_TASK_BLOCKED：在延时表或事件等待链表中；
 * - HT_TASK_SUSPENDED：被 htTaskSuspend() 挂起；
 * - HT_TASK_INVALID：传入的句柄无效。
 */
enum htTaskStatus {
	HT_TASK_READY = 0, /**< 任务就绪 */
	HT_TASK_RUNNING, /**< 任务运行中 */
	HT_TASK_BLOCKED, /**< 任务阻塞 */
	HT_TASK_SUSPENDED, /**< 任务挂起 */
	HT_TASK_INVALID, /**< 无效任务句柄 */
};
/** @brief 任务状态类型别名。 */
typedef enum htTaskStatus htTaskStatus_t;

/**
 * @brief 调度器状态。
 * - HT_SCHEDULER_NOT_STARTED：尚未启动调度；
 * - HT_SCHEDULER_RUNNING：正常调度中；
 * - HT_SCHEDULER_SUSPENDED：调度被暂停，仅置起切换请求而不真正切换。
 */
enum htSchedulerState { HT_SCHEDULER_NOT_STARTED = 0, HT_SCHEDULER_RUNNING, HT_SCHEDULER_SUSPENDED };
/** @brief 调度器状态类型别名。 */
typedef enum htSchedulerState htSchedulerState_t;

/**
 * @brief 内置的任务优先级档位（实际可用范围由 configMAX_PRIORITIES 决定）。
 * HT_IDLE_TASK 供空闲任务使用，数值越大优先级越高。
 */
enum htTaskPriority { HT_IDLE_TASK = 0, HT_LOW_TASK, HT_NORMAL_TASK, HT_HIGH_TASK };

/* ------------------------------------------------------------------ */
/* 4. 队列类型与阻塞常量                                                */
/* ------------------------------------------------------------------ */

/** @brief 队列类型基值，仅用于类型标记。 */
#define htQUEUE_TYPE_BASE 0U
/** @brief 普通互斥量类型标记。 */
#define htQUEUE_TYPE_MUTEX 1U
/** @brief 计数信号量类型标记。 */
#define htQUEUE_TYPE_COUNTING_SEMAPHORE 2U
/** @brief 二值信号量类型标记。 */
#define htQUEUE_TYPE_BINARY_SEMAPHORE 3U
/** @brief 递归互斥量类型标记。 */
#define htQUEUE_TYPE_RECURSIVE_MUTEX 4U

/**
 * @brief 无限阻塞的超时值。
 *
 * 传给所有 take/receive 类接口的 xTicksToWait 参数表示永久等待，不会超时。
 */
#define htBLOCKED_INDEFINITELY ((TickType_t)0xFFFFFFFFUL)

/* ------------------------------------------------------------------ */
/* 5. 链表结构                                                          */
/* ------------------------------------------------------------------ */

struct htList;

/**
 * @brief 双向循环链表的一个完整节点。
 *
 * 节点直接内嵌在任务 TCB 等对象中，内核不为链表单独分配内存。
 */
typedef struct htListItem {
	TickType_t xItemValue; /**< 排序值：就绪表用优先级、延时表用唤醒 tick */
	struct htListItem *pxNext; /**< 下一个节点 */
	struct htListItem *pxPrevious; /**< 上一个节点 */
	void *pvOwner; /**< 节点所有者，通常是它所在任务的 TCB */
	struct htList *pxContainer; /**< 节点当前所属的链表，NULL 表示未挂入任何链表 */
} htListItem_t;

/**
 * @brief 精简链表尾哨兵节点（不含 pvOwner / pxContainer），用于压缩每个 htList_t 的内存占用。
 */
typedef struct htMiniListItem {
	TickType_t xItemValue; /**< 尾哨兵排序值，固定为 UINT32_MAX */
	struct htListItem *pxNext; /**< 指向链表首节点，空表时指向哨兵自身 */
	struct htListItem *pxPrevious; /**< 指向链表尾节点，空表时指向哨兵自身 */
} htMiniListItem_t;

/**
 * @brief 带精简哨兵的双向循环链表（32 位目标仅占 20 字节）。
 *
 * 头尾通过固定的 xListEnd 精简哨兵相连，遍历结束条件为节点等于哨兵地址。
 */
typedef struct htList {
	UBaseType_t uxNumberOfItems; /**< 当前节点数量 */
	htListItem_t *pxIndex; /**< 遍历索引，轮转调度靠它指向下一个候选任务 */
	htMiniListItem_t xListEnd; /**< 精简尾哨兵，xItemValue 固定为 0xFFFFFFFF */
} htList_t;

#endif /* HT_TYPES_H */
