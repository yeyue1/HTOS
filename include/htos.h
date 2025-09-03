/**
 * @file htOS.h
 * @brief 自定义操作系统API定义
 */
#ifndef HT_OS_H
#define HT_OS_H

#include <stdint.h>
#include "httask.h"      // 包含httask.h以使用其定义
#include "htqueue.h"     // 添加对队列的引用
#include "htsemaphore.h" // 添加对信号量的引用

/** 系统时钟类型定义 */
typedef uint32_t TickType_t;

/**
 * 操作系统初始化
 * 
 * @return 0表示成功，非0表示失败
 */
int htOSInit(void);

/**
 * 启动操作系统调度器
 * 此函数通常不会返回
 * 
 * @return 0表示成功，非0表示失败（如果返回）
 */
int htOSStart(void);



/**
 * 创建新任务（兼容旧API）
 * 
 * @param name 任务名称
 * @param function 任务函数
 * @param param 传递给任务的参数
 * @param stackSize 任务栈大小（字节）
 * @param priority 任务优先级
 * @return 任务句柄，NULL表示失败
 */
TaskHandle_t htOSTaskCreate(const char* name, void (*function)(void*), void* param, 
                  uint32_t stackSize, uint8_t priority);

/**
 * 删除任务（兼容旧API）
 * 
 * @param task 任务句柄
 * @return 0表示成功，非0表示失败
 */
int htOSTaskDelete(TaskHandle_t task);

/**
 * 创建队列（兼容旧API）
 * 
 * @param length 队列长度（项目数量） 
 * @param itemSize 每个项目的大小（字节）
 * @return 队列句柄，NULL表示失败
 */
QueueHandle_t htOSQueueCreate(uint32_t length, uint32_t itemSize);

/**
 * 发送数据到队列（兼容旧API）
 * 
 * @param queue 队列句柄
 * @param item 要发送的数据指针
 * @param timeout 等待超时时间（毫秒）
 * @return 0表示成功，非0表示失败
 */
int htOSQueueSend(QueueHandle_t queue, const void* item, uint32_t timeout);

/**
 * 从队列接收数据（兼容旧API）
 * 
 * @param queue 队列句柄 
 * @param buffer 接收数据的缓冲区指针
 * @param timeout 等待超时时间（毫秒）
 * @return 0表示成功，非0表示失败
 */
int htOSQueueReceive(QueueHandle_t queue, void* buffer, uint32_t timeout);

/**
 * 创建二值信号量（兼容旧API）
 * 
 * @return 信号量句柄，NULL表示失败
 */
SemaphoreHandle_t htOSSemaphoreCreateBinary(void);

/**
 * 创建互斥锁（兼容旧API）
 * 
 * @return 互斥锁句柄，NULL表示失败
 */
SemaphoreHandle_t htOSMutexCreate(void);

/**
 * 获取信号量/互斥锁（兼容旧API）
 * 
 * @param sem 信号量/互斥锁句柄
 * @param timeout 等待超时时间（毫秒）
 * @return 0表示成功，非0表示失败
 */
int htOSSemaphoreTake(SemaphoreHandle_t sem, uint32_t timeout);

/**
 * 释放信号量/互斥锁（兼容旧API）
 * 
 * @param sem 信号量/互斥锁句柄
 * @return 0表示成功，非0表示失败
 */
int htOSSemaphoreGive(SemaphoreHandle_t sem);

#endif /* HT_OS_H */
