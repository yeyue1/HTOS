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

/* 栈溢出检查：每次切换前检查即将被换下的任务栈底 4 个字是否仍为填充值，
 * 被写穿则调用 htStackOverflowHook()。置 0 可把相关代码从固件里裁掉。 */
#define configCHECK_FOR_STACK_OVERFLOW 1 /* 1=启用栈溢出检查 */

/* 钩子函数配置 */
#define configUSE_IDLE_HOOK 0 /* 使用空闲钩子 */
#define configUSE_TICK_HOOK 0 /* 使用滴答钩子 */
#define configUSE_MALLOC_FAILED_HOOK 0 /* 使用内存分配失败钩子 */

/* 功能开关 */
#define configUSE_MUTEXES 1 /* 使用互斥锁 */
#define configUSE_RECURSIVE_MUTEXES 1 /* 使用递归互斥锁 */
#define configUSE_COUNTING_SEMAPHORES 1 /* 使用计数信号量 */
#define configUSE_QUEUE_SETS 0 /* 使用队列集 */
#define configUSE_TASK_NOTIFICATIONS 1 /* 启用任务通知 API */
#define configSUPPORT_STATIC_ALLOCATION 1 /* 支持调用方提供任务和队列内存 */

/* 空闲任务配置 */
#define configDEBUG_IDLE_STATS 0 /* 启用空闲任务状态打印 */

/*
 * 配置审计
 *
 * 下面这些开关在加入本注释时还没有任何 #if 引用它们：无论设成 0 还是 1，
 * 生成的固件都完全一样。配置项静默失效比缺少该功能更难排查——使用者会以为
 * 自己已经关掉了某项功能去省 Flash，或者以为打开了某个钩子。这里让它们在
 * 编译期直接失败，把"没有实现"变成一条明确的编译错误。
 *
 * 实现其中某项后，把对应的 #if 删掉即可。
 */
#if (configUSE_QUEUE_SETS != 0)
#error "configUSE_QUEUE_SETS: queue sets are not implemented"
#endif

#if (configGENERATE_RUN_TIME_STATS != 0)
#error "configGENERATE_RUN_TIME_STATS: runtime stats are stubs, htGetCPUUsage() returns 0"
#endif

#if (configUSE_STATS_FORMATTING_FUNCTIONS != 0)
#error "configUSE_STATS_FORMATTING_FUNCTIONS: needs runtime stats, which are not implemented"
#endif

#if (configUSE_TICK_HOOK != 0)
#error "configUSE_TICK_HOOK: the hook is never called"
#endif

#if (configUSE_MALLOC_FAILED_HOOK != 0)
#error "configUSE_MALLOC_FAILED_HOOK: the hook is never called"
#endif

/*
 * 已实现但尚未用 #if 包裹的开关：功能可用，但把它们设为 0 不会裁掉任何代码。
 * configUSE_TRACE_FACILITY 同理，它当前不控制任何实现。
 */
/* configUSE_MUTEXES, configUSE_COUNTING_SEMAPHORES, configUSE_TRACE_FACILITY */

#endif /* HT_CONFIG_H */
