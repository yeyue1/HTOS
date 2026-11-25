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
#ifndef configTICK_RATE_HZ
#define configTICK_RATE_HZ 1000
#endif

/**
 * CPU 主频，单位 Hz，用于把 DWT 周期数换算成时间。
 *
 * 本仓库固件按 72 MHz 运行；这个值只参与"周期 -> 微秒"的除法（72 MHz 时正好 72 个
 * 周期一微秒），不影响任何调度行为。用 #ifndef 包裹，换主频的板子可以用 -D 覆盖。
 */
#ifndef configCPU_CLOCK_HZ
#define configCPU_CLOCK_HZ 72000000UL
#endif

/** 最大任务优先级个数；合法优先级为 0 ~ configMAX_PRIORITIES-1，数值越大优先级越高。 */
#ifndef configMAX_PRIORITIES
#define configMAX_PRIORITIES 32
#endif

/**
 * 任务栈最小深度，单位为 word（Cortex-M 上 1 word = 4 字节）。
 * htTaskCreate() 会把更小的栈请求静默抬到该值，传 0 也不会得到零栈任务。
 */
#ifndef configMINIMAL_STACK_SIZE
#define configMINIMAL_STACK_SIZE 128
#endif

/** 任务名字符串最大长度（含结尾 '\0'），超长名字会被截断。 */
#ifndef configMAX_TASK_NAME_LEN
#define configMAX_TASK_NAME_LEN 16
#endif

/** 1 = 抢占式调度：滴答中断里若有更高优先级任务就绪，立即抢占当前任务。 */
#ifndef configUSE_PREEMPTION
#define configUSE_PREEMPTION 1
#endif

/** 1 = 同优先级任务按时间片轮转；依赖 configUSE_PREEMPTION，关闭抢占后本项无效。 */
#ifndef configUSE_TIME_SLICING
#define configUSE_TIME_SLICING 1
#endif

/**
 * 1 = Cortex-M 临界区采用 BASEPRI 分级屏蔽：仅屏蔽逻辑优先级 >= configMAX_SYSCALL_INTERRUPT_PRIORITY
 * 的中断，高于该阈值（数值更小，如 0..4）的硬实时外设中断永不被内核屏蔽（零抖动）。
 * 0 = 退回 PRIMASK 全局关中断。
 */
#ifndef configUSE_BASEPRI_CRITICAL
#define configUSE_BASEPRI_CRITICAL 1
#endif

/**
 * 允许调用内核 *FromISR API 的最高中断逻辑优先级（数值越小优先级越高）。
 * STM32F103 为 4 位优先级（0..15），默认 5 表示优先级 0..4 为零延迟非 RTOS 中断，
 * 优先级 5..15（含 SysTick=15、PendSV=15）受临界区保护并可安全调用 FromISR 接口。
 */
#ifndef configMAX_SYSCALL_INTERRUPT_PRIORITY
#define configMAX_SYSCALL_INTERRUPT_PRIORITY 5U
#endif

/* ------------------------------------------------------------------ */
/* 2. 内存管理                                                         */
/* ------------------------------------------------------------------ */

/**
 * TLSF 堆总大小，单位字节（12 * 1024 = 12KB）。
 * 任务栈、TCB、队列存储都从这里分配；置 0 时内核编译为纯静态分配版本。
 *
 * 本项可用 -DconfigTOTAL_HEAP_SIZE=... 在构建层覆盖：CMakeLists.txt 的
 * HTOS_HEAP_BYTES 缓存变量就是通过这条路径注入的。之所以单独给它加
 * #ifndef，是因为这是唯一一个"不同工程需要不同取值"的配置——库默认
 * 保留 12KB 以兼容动态创建的既有用例，而本仓库的 app/ 已全部改用
 * htTaskCreateStatic()，固件侧把 HTOS_HEAP_BYTES 设 0 即可整块回收。
 */
#ifndef configTOTAL_HEAP_SIZE
#define configTOTAL_HEAP_SIZE (12 * 1024)
#endif

/* ------------------------------------------------------------------ */
/* 3. 调试与统计                                                       */
/* ------------------------------------------------------------------ */

/** 1 = 编入全局任务总表 pxAllocatedTasksList 与跟踪字段（TCB.xAllListItem）。 */
#ifndef configUSE_TRACE_FACILITY
#define configUSE_TRACE_FACILITY 1
#endif

/**
 * 1 = 编入运行时统计：每毫秒在 SysTick 内部采一次 DWT 周期计数器，把这段时间记到
 * 当前任务头上，同时累计总时长。htGetCPUUsage()、htGetIdleRunTimePercent()、
 * htGetRunTimeCounter() 因此返回真实数值；置 0 时三者恒返回 0，采样代码与 TCB
 * 的 ullRunTimeCounter 字段整体裁掉。
 */
#ifndef configGENERATE_RUN_TIME_STATS
#define configGENERATE_RUN_TIME_STATS 1
#endif

#if (configGENERATE_RUN_TIME_STATS == 1)
/* 运行时统计统一通过移植层 htPort.h 的周期计数原语访问硬件计数器，内核头不再直连 CMSIS 寄存器。 */
#define portCONFIGURE_TIMER_FOR_RUN_TIME_STATS() htPortCycleCounterInit()
#define portGET_RUN_TIME_COUNTER_VALUE() htPortCycleCounterRead()
#endif

/**
 * 1 = 编入实时性仪表（见 htlatency.h）：在 htEnterCritical()/htExitCritical() 与
 * htTaskTickInc() 里用 DWT 周期计数器打点，记录最坏情况临界区与滴答中断耗时、
 * 峰值出现时的调用点、采样次数，以及一张按 1/2/4/8/16/32/64 us 分档的直方图。
 */
#ifndef configUSE_LATENCY_STATS
#define configUSE_LATENCY_STATS 0
#endif

/**
 * 1 = 编入 htTaskList()：把全部任务的名称、状态、优先级、栈高水位与累计运行时间
 * 拼成一张文本表写进调用者给的缓冲区。内部不调用 printf，超出缓冲区的部分直接截断。
 * 置 0（默认）时整块代码被裁掉，不占 Flash；需要时可用 -D 在构建层打开。
 */
#ifndef configUSE_STATS_FORMATTING_FUNCTIONS
#define configUSE_STATS_FORMATTING_FUNCTIONS 0
#endif

/**
 * 栈溢出检查：每次任务切换前检查即将被换下的任务栈底 4 个字是否仍为填充值，
 * 被写穿则调用 htStackOverflowHook()。置 0 可把相关代码从固件里裁掉。
 */
#ifndef configCHECK_FOR_STACK_OVERFLOW
#define configCHECK_FOR_STACK_OVERFLOW 1
#endif

/* ------------------------------------------------------------------ */
/* 4. 钩子函数                                                         */
/* ------------------------------------------------------------------ */

/** 1 = 空闲任务每轮调用一次 vApplicationIdleHook()，调用方需自行提供该函数。 */
#ifndef configUSE_IDLE_HOOK
#define configUSE_IDLE_HOOK 0
#endif

/**
 * 1 = 每个滴答调用一次 htTickHook()。调用点在 SysTick 处理内部、中断处于关闭状态，
 * 因此钩子必须短小且不得调用任何可能阻塞的 API。弱符号，应用提供同名强符号即可接管。
 */
#ifndef configUSE_TICK_HOOK
#define configUSE_TICK_HOOK 0
#endif

/**
 * 1 = htPortMalloc() 因空间不足返回 NULL 时调用一次 htMallocFailedHook()。
 * 纯静态配置（configTOTAL_HEAP_SIZE 为 0）下每次分配都必然失败，钩子同样会调用，
 * 正好用来暴露"以为有堆其实没有"的误用。弱符号，应用提供同名强符号即可接管。
 */
#ifndef configUSE_MALLOC_FAILED_HOOK
#define configUSE_MALLOC_FAILED_HOOK 0
#endif

/* ------------------------------------------------------------------ */
/* 5. 功能开关                                                         */
/* ------------------------------------------------------------------ */

/** 1 = 提供互斥量接口（htSemaphoreCreateMutex 系列）。 */
#ifndef configUSE_MUTEXES
#define configUSE_MUTEXES 1
#endif

/** 1 = 提供递归互斥量接口，并启用其优先级继承逻辑。 */
#ifndef configUSE_RECURSIVE_MUTEXES
#define configUSE_RECURSIVE_MUTEXES 1
#endif

/** 1 = 提供计数信号量接口（htSemaphoreCreateCounting 系列）。 */
#ifndef configUSE_COUNTING_SEMAPHORES
#define configUSE_COUNTING_SEMAPHORES 1
#endif

/**
 * 1 = 编入队列集（htQueueSetCreate 系列）：把多个队列/信号量聚成一个集合，
 * 用一次 htQueueSelectFromSet() 等待其中任意一个有数据，而不是逐个轮询。
 * 置 0 时相关 API 与每个队列上的集合字段一起被裁掉。
 */
#ifndef configUSE_QUEUE_SETS
#define configUSE_QUEUE_SETS 1
#endif

/** 1 = 编入任务通知 API（htTaskNotify* 系列）及 TCB 通知字段。 */
#ifndef configUSE_TASK_NOTIFICATIONS
#define configUSE_TASK_NOTIFICATIONS 1
#endif

/** 1 = 支持调用方自带内存的静态创建接口（htTaskCreateStatic / htQueueCreateStatic）。 */
#ifndef configSUPPORT_STATIC_ALLOCATION
#define configSUPPORT_STATIC_ALLOCATION 1
#endif

/**
 * 1 = 编入软件定时器服务（httimer.h / httimer.c）：单次与周期自动重载定时器、
 * 命令队列与守护任务。默认关闭（0），关闭时零 RAM/Flash 开销。
 */
#ifndef configUSE_TIMERS
#define configUSE_TIMERS 0
#endif

/** 软件定时器守护任务优先级（需小于 configMAX_PRIORITIES）。 */
#ifndef configTIMER_TASK_PRIORITY
#define configTIMER_TASK_PRIORITY 3U
#endif

/** 软件定时器命令队列深度（可缓冲的 Start/Stop/Reset/ChangePeriod/Delete 命令数）。 */
#ifndef configTIMER_QUEUE_LENGTH
#define configTIMER_QUEUE_LENGTH 8U
#endif

/** 软件定时器守护任务栈深度（单位 StackType_t word）。 */
#ifndef configTIMER_TASK_STACK_DEPTH
#define configTIMER_TASK_STACK_DEPTH configMINIMAL_STACK_SIZE
#endif

/**
 * 1 = 编入事件标志组（hteventgroup.h / hteventgroup.c）：24 位事件掩码、
 * ANY/ALL 组合等待与广播唤醒。默认关闭（0），关闭时零 RAM/Flash 开销。
 */
#ifndef configUSE_EVENT_GROUPS
#define configUSE_EVENT_GROUPS 0
#endif

/**
 * 1 = 编入定长内存块池（htpool.h / htpool.c）：O(1) 零碎片确定性分配器，
 * 适用于网络包、传感器帧等高频定长分配。默认关闭（0），关闭时零 RAM/Flash 开销。
 */
#ifndef configUSE_BLOCK_POOL
#define configUSE_BLOCK_POOL 0
#endif

/* ------------------------------------------------------------------ */
/* 6. 调度策略                                                         */
/* ------------------------------------------------------------------ */

/**
 * 1 = 编入公平分享策略（htSchedPolicyFairShare）：优先级语义不变，同一优先级内
 * 按权重分配 CPU，权重用 htSchedSetWeight() 设置，1024 为标准份额。
 * 置 0 时整块代码被裁掉，TCB 也不会多出两个策略字段。默认关闭。
 */
#ifndef configUSE_SCHED_POLICY_FAIR_SHARE
#define configUSE_SCHED_POLICY_FAIR_SHARE 0
#endif

/**
 * 1 = 编入最早死期优先策略（htSchedPolicyEdf）：就绪任务按绝对死期排序，
 * 死期最近者先跑；没有登记死期的任务退化为固定优先级。默认关闭。
 */
#ifndef configUSE_SCHED_POLICY_EDF
#define configUSE_SCHED_POLICY_EDF 0
#endif

/* ------------------------------------------------------------------ */
/* 7. 空闲任务调试                                                     */
/* ------------------------------------------------------------------ */

/** 1 = 空闲任务每循环 100000 次打印一次当前 tick 计数，仅供调试。 */
#ifndef configDEBUG_IDLE_STATS
#define configDEBUG_IDLE_STATS 0
#endif

/**
 * @brief 配置审计：跨开关依赖的编译期强制校验。
 */

/* 格式化函数要遍历全局任务表，因此依赖 configUSE_TRACE_FACILITY。 */
#if (configUSE_STATS_FORMATTING_FUNCTIONS == 1) && (configUSE_TRACE_FACILITY == 0)
#error "configUSE_STATS_FORMATTING_FUNCTIONS requires configUSE_TRACE_FACILITY"
#endif

/* 递归互斥量依赖普通互斥量基础开关。 */
#if (configUSE_RECURSIVE_MUTEXES == 1) && (configUSE_MUTEXES == 0)
#error "configUSE_RECURSIVE_MUTEXES requires configUSE_MUTEXES"
#endif

/* 公平分享与最早死期策略初始化时需遍历全局任务总表 pxAllocatedTasksList。 */
#if ((configUSE_SCHED_POLICY_FAIR_SHARE == 1) || (configUSE_SCHED_POLICY_EDF == 1)) && (configUSE_TRACE_FACILITY == 0)
#error "Fair-share and EDF scheduling policies require configUSE_TRACE_FACILITY"
#endif

#endif /* HT_CONFIG_H */
