/**
 * @file htpool.h
 * @brief 确定性定长内存块池接口（受 configUSE_BLOCK_POOL 控制，默认关闭零开销）。
 *
 * 采用侵入式单向空闲链表实现严格 O(1) 分配与释放，零外部碎片、零每块额外头开销，
 * 并在释放时校验地址范围与块步长对齐，支持任务上下文与中断上下文（FromISR）。
 */
#ifndef HT_POOL_H
#define HT_POOL_H

#include <stddef.h>
#include <stdint.h>
#include "httypes.h"

/** @brief 块池对齐粒度（8 字节 AAPCS 对齐）。 */
#define HT_POOL_ALIGN_SIZE ((size_t)8U)

/**
 * @brief 定长内存块池控制块。
 */
typedef struct htPool {
	void *pvFreeList; /**< 侵入式空闲块链表头 */
	uint8_t *pucPoolStart; /**< 对齐后的池首地址 */
	uint8_t *pucPoolEnd; /**< 最后一个有效块末尾地址 */
	size_t uxBlockSize; /**< 对齐后的单块步长（字节） */
	UBaseType_t uxTotalBlocks; /**< 池中总块数 */
	UBaseType_t uxFreeBlocks; /**< 当前可用空闲块数 */
	UBaseType_t uxMinFreeBlocks; /**< 历史最小空闲块数（峰值压力水位） */
} htPool_t;

#if (configUSE_BLOCK_POOL == 1)

/**
 * @brief 使用调用方提供的连续缓冲区初始化定长块池。
 *
 * @param[out] pxPool 块池控制块。
 * @param[in] pvStorage 原始连续缓冲区。
 * @param[in] uxStorageSize 原始缓冲区总字节数。
 * @param[in] uxBlockSize 请求的单块字节数（内部向上对齐到 max(8, sizeof(void *))）。
 * @return 至少切出 1 块返回 htPASS，参数非法或空间不足返回 htFAIL。
 */
BaseType_t htPoolInit(htPool_t *pxPool, void *pvStorage, size_t uxStorageSize, size_t uxBlockSize);

/** @brief 从块池分配一个定长块（任务上下文临界区保护），无空闲块返回 NULL。 */
void *htPoolAlloc(htPool_t *pxPool);
/** @brief 在中断上下文从块池分配一个定长块（ISR 临界区保护）。 */
void *htPoolAllocFromISR(htPool_t *pxPool);

/**
 * @brief 将块归还到块池（任务上下文）。
 *
 * @return 指针属于本池且满足步长对齐返回 htPASS，非法指针返回 htFAIL。
 */
BaseType_t htPoolFree(htPool_t *pxPool, void *pvBlock);
/** @brief 在中断上下文将块归还到块池。 */
BaseType_t htPoolFreeFromISR(htPool_t *pxPool, void *pvBlock);

/** @brief 读取当前空闲块数量。 */
UBaseType_t htPoolGetFreeCount(const htPool_t *pxPool);
/** @brief 读取历史最小空闲块数量（用于评估峰值水位）。 */
UBaseType_t htPoolGetMinFreeCount(const htPool_t *pxPool);
/** @brief 读取块池总块数。 */
UBaseType_t htPoolGetTotalCount(const htPool_t *pxPool);
/** @brief 读取对齐后的单块实际步长（字节）。 */
size_t htPoolGetBlockSize(const htPool_t *pxPool);

#endif /* configUSE_BLOCK_POOL == 1 */

#endif /* HT_POOL_H */
