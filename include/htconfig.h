/**
 * @file htconfig.h
 * @brief htOS 编译期配置项。
 *
 * 本文件只包含 #define 与配置合法性检查，不含任何代码。修改任一项后必须整体重新编译；
 * 下面对"尚未实现"的开关用 #error 强制拦截，避免改了配置却静默不生效。
 */
#ifndef HT_CONFIG_H
#define HT_CONFIG_H

/* ------------------------------------------------------------------ */
/* 1. 系统时钟与任务规模                                               */
/* ------------------------------------------------------------------ */

/** 系统滴答频率，单位 Hz；htTimeToTicks()/htTicksToTime() 依赖它换算毫秒与滴答。 */
#define configTICK_RATE_HZ 1000

/** 最大任务优先级个数；合法优先级为 0 ~ configMAX_PRIORITIES-1，数值越大优先级越高。 */
#define configMAX_PRIORITIES 32

/**
 * 任务栈最小深度，单位为 word（Cortex-M 上 1 word = 4 字节）。
 * htTaskCreate() 会把更小的栈请求静默抬到该值，传 0 也不会得到零栈任务。
 */
#define configMINIMAL_STACK_SIZE 128

/** 任务名字符串最大长度（含结尾 '\0'），超长名字会被截断。 */
#define configMAX_TASK_NAME_LEN 16

/** 1 = 抢占式调度：滴答中断里若有更高优先级任务就绪，立即抢占当前任务。 */
#define configUSE_PREEMPTION 1

/** 1 = 同优先级任务按时间片轮转；依赖 configUSE_PREEMPTION，关闭抢占后本项无效。 */
#define configUSE_TIME_SLICING 1

/* ------------------------------------------------------------------ */
/* 2. 内存管理                                                         */
/* ------------------------------------------------------------------ */

/**
 * TLSF 堆总大小，单位字节（12 * 1024 = 12KB）。
 * 任务栈、TCB、队列存储都从这里分配；置 0 时内核编译为纯静态分配版本。
 */
#define configTOTAL_HEAP_SIZE (12 * 1024)

/* ------------------------------------------------------------------ */
/* 3. 调试与统计                                                       */
/* ------------------------------------------------------------------ */

/** 1 = 编入跟踪相关字段（如 TCB 的 ulRunTimeCounter）；当前不控制任何实现分支。 */
#define configUSE_TRACE_FACILITY 1

/** 运行时统计开关；当前为桩实现，htGetCPUUsage() 恒返回 0，置 1 会在下方触发 #error。 */
#define configGENERATE_RUN_TIME_STATS 0

/** 统计信息格式化函数开关；依赖运行时统计，置 1 会在下方触发 #error。 */
#define configUSE_STATS_FORMATTING_FUNCTIONS 0

/**
 * 栈溢出检查：每次任务切换前检查即将被换下的任务栈底 4 个字是否仍为填充值，
 * 被写穿则调用 htStackOverflowHook()。置 0 可把相关代码从固件里裁掉。
 */
#define configCHECK_FOR_STACK_OVERFLOW 1

/* ------------------------------------------------------------------ */
/* 4. 钩子函数                                                         */
/* ------------------------------------------------------------------ */

/** 1 = 空闲任务每轮调用一次 vApplicationIdleHook()，调用方需自行提供该函数。 */
#define configUSE_IDLE_HOOK 0

/** 滴答钩子开关；当前实现从不调用该钩子，置 1 会在下方触发 #error。 */
#define configUSE_TICK_HOOK 0

/** 内存分配失败钩子开关；当前实现从不调用该钩子，置 1 会在下方触发 #error。 */
#define configUSE_MALLOC_FAILED_HOOK 0

/* ------------------------------------------------------------------ */
/* 5. 功能开关                                                         */
/* ------------------------------------------------------------------ */

/** 1 = 提供互斥量接口（htSemaphoreCreateMutex 系列）。 */
#define configUSE_MUTEXES 1

/** 1 = 提供递归互斥量接口，并启用其优先级继承逻辑。 */
#define configUSE_RECURSIVE_MUTEXES 1

/** 1 = 提供计数信号量接口（htSemaphoreCreateCounting 系列）。 */
#define configUSE_COUNTING_SEMAPHORES 1

/**
 * 1 = 编入队列集（htQueueSetCreate 系列）：把多个队列/信号量聚成一个集合，
 * 用一次 htQueueSelectFromSet() 等待其中任意一个有数据，而不是逐个轮询。
 * 置 0 时相关 API 与每个队列上的集合字段一起被裁掉；置 1 时每个队列固定多占 12 字节。
 */
#define configUSE_QUEUE_SETS 1

/** 1 = 编入任务通知 API（htTaskNotify* 系列）。 */
#define configUSE_TASK_NOTIFICATIONS 1

/** 1 = 支持调用方自带内存的静态创建接口（htTaskCreateStatic / htQueueCreateStatic）。 */
#define configSUPPORT_STATIC_ALLOCATION 1

/* ------------------------------------------------------------------ */
/* 6. 空闲任务调试                                                     */
/* ------------------------------------------------------------------ */

/** 1 = 空闲任务每循环 100000 次打印一次当前 tick 计数，仅供调试。 */
#define configDEBUG_IDLE_STATS 0

/**
 * @brief 配置审计：把"设了也没有实现"的开关变成编译期错误。
 *
 * 下面这些开关目前还没有任何 #if 引用它们：无论设成 0 还是 1，生成的固件都完全一样。
 * 配置项静默失效比缺少该功能更难排查——使用者会以为自己已经关掉了某项功能去省 Flash，
 * 或者以为打开了某个钩子。这里让它们在编译期直接失败，把"没有实现"变成一条明确的编译错误。
 *
 * 实现其中某项后，把对应的 #if 删掉即可；configUSE_QUEUE_SETS 就是这样摘掉拦截的。
 */
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

/**
 * @note 已实现但尚未用 #if 包裹的开关：功能可用，但把它们设为 0 不会裁掉任何代码。
 * configUSE_TRACE_FACILITY 同理，它当前不控制任何实现。
 *
 * 涉及：configUSE_MUTEXES、configUSE_COUNTING_SEMAPHORES、configUSE_TRACE_FACILITY。
 */
/* configUSE_MUTEXES, configUSE_COUNTING_SEMAPHORES, configUSE_TRACE_FACILITY */

#endif /* HT_CONFIG_H */
