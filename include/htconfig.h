/**
 * @file htconfig.h
 * @brief htOS配置文件
 */
#ifndef HT_CONFIG_H
#define HT_CONFIG_H

/* 系统时钟配置 */
#define configTICK_RATE_HZ 1000 /* 系统滴答频率，单位Hz */
#define configMAX_PRIORITIES 32 /* 最大支持的任务优先级 */
#define configMINIMAL_STACK_SIZE 128 /* 最小任务栈大小(字) */
#define configMAX_TASK_NAME_LEN 16 /* 任务名最大长度 */
#define configUSE_PREEMPTION 1 /* 使用抢占式调度 */
#define configUSE_TIME_SLICING 1 /* 使用时间片调度 */

/* 内存管理配置 - 调整堆内存大小 */
#define configTOTAL_HEAP_SIZE (12 * 1024) /*12KB */

/* 调试配置 */
#define configUSE_TRACE_FACILITY 1 /* 使用跟踪功能 */
#define configGENERATE_RUN_TIME_STATS 0 /* 生成运行时统计信息 */
#define configUSE_STATS_FORMATTING_FUNCTIONS 0 /* 使用统计信息格式化函数 */

/* 钩子函数配置 */
#define configUSE_IDLE_HOOK 0 /* 使用空闲钩子 */
#define configUSE_TICK_HOOK 0 /* 使用滴答钩子 */
#define configUSE_MALLOC_FAILED_HOOK 0 /* 使用内存分配失败钩子 */

/* 功能开关 */
#define configUSE_MUTEXES 1 /* 使用互斥锁 */
#define configUSE_RECURSIVE_MUTEXES 1 /* 使用递归互斥锁 */
#define configUSE_COUNTING_SEMAPHORES 1 /* 使用计数信号量 */
#define configUSE_QUEUE_SETS 0 /* 使用队列集 */
#define configUSE_TASK_NOTIFICATIONS 1 /* 使用任务通知 */

/* 空闲任务配置 */
#define configDEBUG_IDLE_STATS 0 /* 启用空闲任务状态打印 */

#endif /* HT_CONFIG_H */
