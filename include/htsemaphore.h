/**
 * @file htsemaphore.h
 * @brief 信号量与互斥量接口。
 *
 * 信号量完全复用队列实现：每项字节数为 0 的队列即计数信号量（只累加计数、
 * 不搬运数据），因此二值信号量就是上限为 1 的计数信号量，互斥量是不带优先级继承的
 * 上限为 1 的二值令牌；递归互斥量则把持有者信息当作数据项存放在队列里，
 * 从而能记录递归层数并做优先级继承。
 */
#ifndef HT_SEMAPHORE_H
#define HT_SEMAPHORE_H

#include "httypes.h"
#include "htqueue.h"
#include "httask.h"

/** @brief 信号量句柄，直接复用队列句柄。 */
typedef QueueHandle_t SemaphoreHandle_t;

/**
 * @brief 动态创建二值信号量，初始为不可用状态。
 *
 * @return 信号量句柄，内存不足返回 NULL。
 * @note 上限为 1，重复释放会失败；取走后需再次释放才能继续同步。
 */
SemaphoreHandle_t htSemaphoreCreateBinary(void);
/**
 * @brief 用调用方提供的控制块静态创建二值信号量。
 *
 * @param[in] pxQueueBuffer 调用方提供的队列控制块。
 * @return 信号量句柄，参数非法返回 NULL。
 */
SemaphoreHandle_t htSemaphoreCreateBinaryStatic(htQUEUE_t *pxQueueBuffer);

/**
 * @brief 动态创建计数信号量。
 *
 * @param[in] uxMaxCount 计数上限，必须大于 0。
 * @param[in] uxInitialCount 初始计数值，不得大于上限。
 * @return 信号量句柄，参数非法或内存不足返回 NULL。
 */
SemaphoreHandle_t htSemaphoreCreateCounting(UBaseType_t uxMaxCount, UBaseType_t uxInitialCount);
/**
 * @brief 用调用方提供的控制块静态创建计数信号量。
 *
 * @param[in] uxMaxCount 计数上限。
 * @param[in] uxInitialCount 初始计数值。
 * @param[in] pxQueueBuffer 调用方提供的队列控制块。
 * @return 信号量句柄，参数非法返回 NULL。
 */
SemaphoreHandle_t htSemaphoreCreateCountingStatic(
		UBaseType_t uxMaxCount, UBaseType_t uxInitialCount, htQUEUE_t *pxQueueBuffer);

/**
 * @brief 动态创建普通互斥量，初始为可用状态。
 *
 * @return 互斥量句柄，内存不足返回 NULL。
 * @note 普通互斥量是上限为 1 的二值令牌，不支持递归获取、也不做优先级继承。
 */
SemaphoreHandle_t htSemaphoreCreateMutex(void);
/**
 * @brief 用调用方提供的控制块静态创建普通互斥量。
 *
 * @param[in] pxQueueBuffer 调用方提供的队列控制块。
 * @return 互斥量句柄，参数非法返回 NULL。
 */
SemaphoreHandle_t htSemaphoreCreateMutexStatic(htQUEUE_t *pxQueueBuffer);
/**
 * @brief 动态创建递归互斥量，初始为可用状态。
 *
 * @return 递归互斥量句柄，内存不足返回 NULL。
 * @note 同一任务可多次获取，内部记录递归层数；持有时会临时提升持有者优先级，
 *       释放到最外层后恢复原优先级。
 */
SemaphoreHandle_t htSemaphoreCreateRecursiveMutex(void);

/**
 * @brief 删除信号量并释放其占用的资源。
 *
 * @param[in] xSemaphore 信号量句柄。
 * @warning 删除前必须保证没有任务阻塞在该信号量上，否则那些任务将永远得不到唤醒。
 */
void htSemaphoreDelete(SemaphoreHandle_t xSemaphore);

/**
 * @brief 获取信号量，不可用时按 tick 超时阻塞等待。
 *
 * @param[in] xSemaphore 信号量句柄。
 * @param[in] xTicksToWait 最长阻塞 tick 数，传 htBLOCKED_INDEFINITELY 表示永久等待。
 * @return htPASS(0) 表示获取成功，htFAIL(-1) 表示超时或参数非法。
 */
BaseType_t htSemaphoreTake(SemaphoreHandle_t xSemaphore, TickType_t xTicksToWait);
/**
 * @brief 中断上下文里获取信号量，不阻塞。
 *
 * @param[in] xSemaphore 信号量句柄。
 * @param[out] pxHigherPriorityTaskWoken 若唤醒的任务优先级更高，会被写为 htTRUE。
 * @return htPASS(0) 表示成功，htFAIL(-1) 表示当前不可用。
 */
BaseType_t htSemaphoreTakeFromISR(SemaphoreHandle_t xSemaphore, BaseType_t *pxHigherPriorityTaskWoken);

/**
 * @brief 获取普通互斥量。
 *
 * @param[in] xSemaphore 互斥量句柄。
 * @param[in] xTicksToWait 最长阻塞 tick 数。
 * @return htPASS(0) 表示成功，htFAIL(-1) 表示超时或参数非法。
 * @note 普通互斥量使用有上限的二值令牌，不提供递归计数。
 */
BaseType_t htSemaphoreTakeMutex(SemaphoreHandle_t xSemaphore, TickType_t xTicksToWait);

/**
 * @brief 获取递归互斥量，同一任务可重入。
 *
 * @param[in] xSemaphore 递归互斥量句柄。
 * @param[in] xTicksToWait 最长阻塞 tick 数。
 * @return htPASS(0) 表示成功，htFAIL(-1) 表示超时或参数非法。
 * @note 若等待超时，会先把已写入的持有者信息撤销再返回失败，不会留下半提交状态。
 */
BaseType_t htSemaphoreTakeRecursive(SemaphoreHandle_t xSemaphore, TickType_t xTicksToWait);

/**
 * @brief 释放信号量。
 *
 * @param[in] xSemaphore 信号量句柄。
 * @return htPASS(0) 表示成功，htFAIL(-1) 表示计数已达上限或参数非法。
 */
BaseType_t htSemaphoreGive(SemaphoreHandle_t xSemaphore);
/**
 * @brief 中断上下文里释放信号量。
 *
 * @param[in] xSemaphore 信号量句柄。
 * @param[out] pxHigherPriorityTaskWoken 若唤醒的任务优先级更高，会被写为 htTRUE。
 * @return htPASS(0) 表示成功，htFAIL(-1) 表示计数已达上限或参数非法。
 */
BaseType_t htSemaphoreGiveFromISR(SemaphoreHandle_t xSemaphore, BaseType_t *pxHigherPriorityTaskWoken);

/**
 * @brief 释放普通互斥量。
 *
 * @param[in] xSemaphore 互斥量句柄。
 * @return htPASS(0) 表示成功，htFAIL(-1) 表示参数非法。
 */
BaseType_t htSemaphoreGiveMutex(SemaphoreHandle_t xSemaphore);

/**
 * @brief 释放递归互斥量，层数减到 0 时才真正归还。
 *
 * @param[in] xSemaphore 递归互斥量句柄。
 * @return htPASS(0) 表示成功，htFAIL(-1) 表示当前任务并未持有它或参数非法。
 */
BaseType_t htSemaphoreGiveRecursive(SemaphoreHandle_t xSemaphore);

/**
 * @brief 读取信号量当前计数值。
 *
 * @param[in] xSemaphore 信号量句柄。
 * @return 当前计数值；句柄为 NULL 时返回 0。
 */
UBaseType_t htSemaphoreGetCount(SemaphoreHandle_t xSemaphore);

#endif /* HT_SEMAPHORE_H */
