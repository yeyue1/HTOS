/**
 * @file htos.h
 * @brief htOS 面向应用层的兼容 API。
 *
 * 本文件把内核原语包装成一套更简单的接口：任务、队列、信号量的创建与操作都用
 * return 0 / -1 与毫秒超时表达，便于教学示例和从其它 RTOS 迁移的代码直接调用。
 * 底层仍由 httask.c / htqueue.c / htsemaphore.c 实现。
 */
#ifndef HT_OS_H
#define HT_OS_H

#include <stdint.h>
/** @brief 任务相关的类型与接口。 */
#include "httask.h"
/** @brief 队列句柄与队列接口。 */
#include "htqueue.h"
/** @brief 信号量句柄与信号量接口。 */
#include "htsemaphore.h"

/**
 * @brief 初始化操作系统内核。
 *
 * 会初始化调度器与内存管理器并清零 tick 计数。重复调用是幂等的。
 *
 * @return 0 表示成功，非 0 表示失败。
 */
int htOSInit(void);

/**
 * @brief 启动调度器，开始多任务运行。
 *
 * 正常启动后本函数不返回，控制权交给第一个就绪任务。
 *
 * @return 仅在启动失败时返回，0 表示成功，非 0 表示失败。
 * @note 调用前必须至少创建一个任务，否则会打印错误并返回 -1。
 */
int htOSStart(void);

/**
 * @brief 创建新任务（兼容旧 API）。
 *
 * @param[in] name 任务名称字符串。
 * @param[in] function 任务入口函数。
 * @param[in] param 传给任务入口的参数。
 * @param[in] stackSize 任务栈大小，单位为字节（内部会换算成 word 数）。
 * @param[in] priority 任务优先级。
 * @return 任务句柄，失败返回 NULL。
 * @note 栈大小小于 configMINIMAL_STACK_SIZE 时会被静默抬到最小值。
 */
TaskHandle_t htOSTaskCreate(
		const char *name, void (*function)(void *), void *param, uint32_t stackSize, uint8_t priority);

/**
 * @brief 删除任务（兼容旧 API）。
 *
 * @param[in] task 任务句柄。
 * @return 0 表示成功，非 0 表示失败；句柄为 NULL 时返回 -1。
 * @warning 删除自身后不会返回，须确保任务不再使用共享资源。
 */
int htOSTaskDelete(TaskHandle_t task);

/**
 * @brief 创建队列（兼容旧 API）。
 *
 * @param[in] length 队列可容纳的项目个数。
 * @param[in] itemSize 每个项目的字节数。
 * @return 队列句柄，失败返回 NULL。
 * @note itemSize 为 0 时得到的是计数信号量，只计数不搬运数据。
 */
QueueHandle_t htOSQueueCreate(uint32_t length, uint32_t itemSize);

/**
 * @brief 向队列发送数据（兼容旧 API）。
 *
 * @param[in] queue 队列句柄。
 * @param[in] item 待发送数据的指针，数据会被复制进队列。
 * @param[in] timeout 等待超时时间，单位毫秒；传 UINT32_MAX 表示永久阻塞。
 * @return 0 表示成功，非 0 表示失败（超时或参数非法）。
 */
int htOSQueueSend(QueueHandle_t queue, const void *item, uint32_t timeout);

/**
 * @brief 从队列接收数据（兼容旧 API）。
 *
 * @param[in] queue 队列句柄。
 * @param[out] buffer 接收缓冲区指针，队列中的数据会复制到这里。
 * @param[in] timeout 等待超时时间，单位毫秒；传 UINT32_MAX 表示永久阻塞。
 * @return 0 表示成功，非 0 表示失败（超时或参数非法）。
 */
int htOSQueueReceive(QueueHandle_t queue, void *buffer, uint32_t timeout);

/**
 * @brief 创建二值信号量（兼容旧 API）。
 *
 * @return 信号量句柄，失败返回 NULL。
 */
SemaphoreHandle_t htOSSemaphoreCreateBinary(void);

/**
 * @brief 创建互斥锁（兼容旧 API）。
 *
 * @return 互斥锁句柄，失败返回 NULL。
 * @note 这里得到的是上限为 1 的二值令牌，普通互斥量不带优先级继承。
 */
SemaphoreHandle_t htOSMutexCreate(void);

/**
 * @brief 获取信号量或互斥锁（兼容旧 API）。
 *
 * @param[in] sem 信号量或互斥锁句柄。
 * @param[in] timeout 等待超时时间，单位毫秒；传 UINT32_MAX 表示永久阻塞。
 * @return 0 表示成功，非 0 表示失败（超时或参数非法）。
 */
int htOSSemaphoreTake(SemaphoreHandle_t sem, uint32_t timeout);

/**
 * @brief 释放信号量或互斥锁（兼容旧 API）。
 *
 * @param[in] sem 信号量或互斥锁句柄。
 * @return 0 表示成功，非 0 表示失败。
 */
int htOSSemaphoreGive(SemaphoreHandle_t sem);

#endif /* HT_OS_H */
