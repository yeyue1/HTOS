/**
 * @file hteventgroup.h
 * @brief 事件标志组接口（受 configUSE_EVENT_GROUPS 控制，默认关闭零开销）。
 *
 * 提供 24 位事件掩码（bit 0..23），支持多任务按 ANY（任意一位）或 ALL（全部位）
 * 阻塞等待，以及 SetBits 时的原子广播唤醒与按需清零。高 8 位由内核用于内部控制标记。
 */
#ifndef HT_EVENT_GROUP_H
#define HT_EVENT_GROUP_H

#include "httypes.h"
#include "htlist.h"

/** @brief 事件位掩码类型（低 24 位有效）。 */
typedef uint32_t EventBits_t;

/** @brief 允许应用使用的 24 位事件掩码。 */
#define HT_EVENT_BITS_MASK ((EventBits_t)0x00FFFFFFUL)

/**
 * @brief 事件标志组控制块，公开布局供 htEventGroupCreateStatic() 静态分配使用。
 */
typedef struct htEventGroupDefinition {
	EventBits_t uxEventBits; /**< 当前已置位的事件标志 */
	htList_t xTasksWaitingForBits; /**< 等待事件标志满足的阻塞任务链表 */
	uint8_t ucStaticallyAllocated; /**< 非 0 表示静态存储，删除时不释放 */
} htEventGroup_t;

/** @brief 事件标志组句柄。 */
typedef htEventGroup_t *EventGroupHandle_t;

#if (configUSE_EVENT_GROUPS == 1)

/** @brief 动态创建事件标志组，初始所有事件位清零。 */
EventGroupHandle_t htEventGroupCreate(void);

/** @brief 使用调用方提供的控制块静态创建事件标志组。 */
EventGroupHandle_t htEventGroupCreateStatic(htEventGroup_t *pxEventGroupBuffer);

/**
 * @brief 阻塞等待指定事件位满足条件。
 *
 * @param[in] xEventGroup 事件组句柄。
 * @param[in] uxBitsToWaitFor 需等待的非零位掩码（仅低 24 位合法）。
 * @param[in] xClearOnExit 满足条件退出时是否清除 uxBitsToWaitFor 对应位。
 * @param[in] xWaitForAllBits htTRUE 表示等待全部位同时置位，htFALSE 表示任意一位即可。
 * @param[in] xTicksToWait 最长等待 tick 数，htBLOCKED_INDEFINITELY 表示永久阻塞。
 * @return 满足条件时（清零前）或超时时的事件位快照；参数非法返回 0。
 */
EventBits_t htEventGroupWaitBits(EventGroupHandle_t xEventGroup, EventBits_t uxBitsToWaitFor, BaseType_t xClearOnExit,
		BaseType_t xWaitForAllBits, TickType_t xTicksToWait);

/**
 * @brief 置位指定事件标志并广播唤醒所有满足条件的等待任务。
 *
 * @param[in] xEventGroup 事件组句柄。
 * @param[in] uxBitsToSet 待置位掩码（仅低 24 位有效）。
 * @return 本次置位并处理完广播唤醒清零后的最新事件位值。
 */
EventBits_t htEventGroupSetBits(EventGroupHandle_t xEventGroup, EventBits_t uxBitsToSet);

/**
 * @brief 在中断上下文置位事件标志并唤醒所有满足条件的等待任务。
 *
 * @param[in] xEventGroup 事件组句柄。
 * @param[in] uxBitsToSet 待置位掩码（仅低 24 位有效）。
 * @param[out] pxHigherPriorityTaskWoken 若有更高优先级任务被唤醒则置 htTRUE，可为 NULL。
 * @return htPASS 表示成功，htFAIL 表示参数非法。
 */
BaseType_t htEventGroupSetBitsFromISR(
		EventGroupHandle_t xEventGroup, EventBits_t uxBitsToSet, BaseType_t *pxHigherPriorityTaskWoken);

/** @brief 清除指定事件位，返回清除前的事件位快照。 */
EventBits_t htEventGroupClearBits(EventGroupHandle_t xEventGroup, EventBits_t uxBitsToClear);
/** @brief 中断上下文清除指定事件位，返回清除前的事件位快照。 */
EventBits_t htEventGroupClearBitsFromISR(EventGroupHandle_t xEventGroup, EventBits_t uxBitsToClear);

/** @brief 读取当前事件位快照。 */
EventBits_t htEventGroupGetBits(EventGroupHandle_t xEventGroup);
/** @brief 中断上下文读取当前事件位快照。 */
EventBits_t htEventGroupGetBitsFromISR(EventGroupHandle_t xEventGroup);

/** @brief 删除事件标志组（静态分配只清零字段，不释放内存）。 */
void htEventGroupDelete(EventGroupHandle_t xEventGroup);

#endif /* configUSE_EVENT_GROUPS == 1 */

#endif /* HT_EVENT_GROUP_H */
