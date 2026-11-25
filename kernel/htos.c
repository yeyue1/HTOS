/**
 * @file htos.c
 * @brief htOS 的统一入口：内核初始化与启动，以及任务/队列/信号量的旧版 API 包装。
 *
 * 这一层刻意做得很薄。真正的调度、队列与信号量逻辑分别在 htscheduler.c、
 * htqueue.c、htsemaphore.c 中实现，本文件只负责参数校验、毫秒到滴答的换算，
 * 以及把内核返回的 htPASS/htFAIL 归一化成宿主风格的 0/-1。
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "htos.h"
#include "httask.h"
#include "htscheduler.h"
#include "htmem.h"
#include "htqueue.h"
#include "htsemaphore.h"
#include "htutils.h"
#include "htlatency.h"
#include "httrace.h"

/** 主版本号。 */
#define HT_OS_VERSION_MAJOR 0
/** 次版本号。 */
#define HT_OS_VERSION_MINOR 1
/** 构建号，预留给后续版本标识使用，当前不参与任何逻辑判断。 */
#define HT_OS_VERSION_BUILD 0

/* 系统滴答计数器：由 httask.c 定义，这里只做外部声明，避免重复定义。 */
extern TickType_t xTickCount;

/** 内核是否已完成初始化：0 表示未初始化，非 0 表示已完成。 */
int gOSInitialized = 0;
/** 调度器是否已启动：0 表示未运行，非 0 表示已进入多任务状态。 */
int gOSRunning = 0;

/**
 * @brief 把毫秒超时换算成内核的滴答超时。
 *
 * @param[in] timeout 超时时间，单位毫秒；UINT32_MAX 是旧版 API 约定的“永久等待”值。
 * @return timeout 为 UINT32_MAX 时原样返回 htBLOCKED_INDEFINITELY，否则返回换算后的滴答数。
 * @note 旧版 API 用 UINT32_MAX 表示无限等待，内核用 htBLOCKED_INDEFINITELY 表示同一语义。
 *       两者当前数值相同，仍然在这里显式转换，避免今后修改宏值时语义悄悄漂移。
 */
static TickType_t prvMillisecondsToTicks(uint32_t timeout)
{
	return timeout == UINT32_MAX ? htBLOCKED_INDEFINITELY : (TickType_t)htTimeToTicks(timeout);
}

/* ------------------------------------------------------------------ */
/* 诊断输出出口                                                       */
/* ------------------------------------------------------------------ */

/**
 * @brief 诊断输出的弱符号默认实现：什么也不做。
 *
 * 内核只声明出口，不决定落地方式。应用若需要看到诊断信息，实现同名强符号即可，
 * 链接器会用强符号覆盖这里的空实现。默认空实现保证内核单独编译、主机构建和
 * 单元测试都不需要额外的桩函数。
 *
 * @param[in] pcMessage 被丢弃的诊断字符串。
 */
__attribute__((weak)) void htTrace(const char *pcMessage)
{
	(void)pcMessage;
}

/**
 * @brief “标签 + 数值”诊断输出的弱符号默认实现：什么也不做。
 *
 * @param[in] pcTag   被丢弃的标签字符串。
 * @param[in] ulValue 被丢弃的数值。
 */
__attribute__((weak)) void htTraceValue(const char *pcTag, uint32_t ulValue)
{
	(void)pcTag;
	(void)ulValue;
}

/**
 * @brief 初始化操作系统内核。
 *
 * 依次完成：清零滴答计数器、初始化调度器数据结构、置位初始化标志、初始化内存分配器。
 *
 * @return 固定返回 0（成功）。
 * @note 可重复调用，第二次起直接返回 0，不会重复初始化。
 * @note 必须在创建任何任务、队列或信号量之前调用；htOSStart() 发现未初始化时也会自动补调。
 */
int htOSInit(void)
{
	if (gOSInitialized) {
		return 0; /* 已经初始化，直接返回。 */
	}

#if (configUSE_LATENCY_STATS == 1)
	/*
	 * 尽早接管 DWT：放在 htSchedulerInit() 之前，这样初始化期间自身的临界区
	 * 也会被采样。关闭该开关时这一整块（含 htLatencyInit 的定义）都不存在。
	 */
	htLatencyInit();
#endif

	/* 清零滴答计数器，保证运行时间从 0 开始累计。 */
	xTickCount = 0;

	/* 初始化调度器：就绪链表、延时链表与当前任务指针。 */
	htSchedulerInit();

	gOSInitialized = 1;
	htMemInit();
	return 0;
}

/**
 * @brief 启动调度器，开始多任务运行。
 *
 * 启动前会检查初始化状态与可运行任务，任何一项不满足都打印诊断信息并返回，
 * 所有诊断输出都带 “[htOS]” 前缀，便于从串口日志定位启动失败的原因。
 *
 * @return 正常启动后不再返回（CPU 交给调度器和任务）；初始化失败、没有可运行任务，
 *         或调度器意外退出时返回 -1。
 * @warning 调用前至少要用 htOSTaskCreate() 创建一个任务，否则会因 pxCurrentTCB 为 NULL 而失败。
 * @note 重复调用是安全的：已经在运行时打印提示并返回 0，不会二次启动。
 */
int htOSStart(void)
{
	if (!gOSInitialized) {
		htTrace("[htOS] System not initialized, initializing now...\r\n");
		if (htOSInit() != 0) {
			htTrace("[htOS] Initialization failed!\r\n");
			return -1;
		}
	}

	/* 已经在运行就直接返回，避免重复启动调度器。 */
	if (gOSRunning) {
		htTrace("[htOS] System already running!\r\n");
		return 0;
	}

	/* 没有任何任务可运行时启动必然失败，提前拦截。 */
	if (pxCurrentTCB == NULL) {
		htTrace("[htOS] ERROR: No tasks available to run!\r\n");
		return -1;
	}

	/* 先置运行标志，再进入调度器。 */
	gOSRunning = 1;

	/* 正常情况下这里不会返回。 */
	htStartScheduler();

	/* 能执行到这里说明调度器异常退出，回滚运行标志并报告失败。 */
	gOSRunning = 0;

	return -1;
}

/**
 * @brief 创建一个任务（旧版 API 包装）。
 *
 * @param[in] name 任务名。
 * @param[in] function 任务入口函数。
 * @param[in] param 传给任务入口函数的参数，可为 NULL。
 * @param[in] stackSize 栈大小，单位字节；内部会折算成 StackType_t 的个数。
 * @param[in] priority 任务优先级，数值越大优先级越高，必须小于 configMAX_PRIORITIES。
 * @return 成功返回任务句柄；参数非法或内存不足时返回 NULL。
 * @note 参数顺序沿用旧版“名字在前”的习惯，与内核 htTaskCreate() 的顺序不同，
 *       转换在内核调用处完成。
 * @note stackSize 不是 sizeof(StackType_t) 的整数倍时，折算会向下取整丢掉余数。
 */
TaskHandle_t htOSTaskCreate(
		const char *name, void (*function)(void *), void *param, uint32_t stackSize, uint8_t priority)
{
	TaskHandle_t taskHandle = NULL;

	/* 调用 httask.h 中的标准任务创建函数：按内核要求的顺序重排参数，
	 * 并把字节数折算成栈深度（单位为 StackType_t）。 */
	BaseType_t status = htTaskCreate(function, /* pxTaskCode */
			name, /* pcName */
			(uint16_t)(stackSize / sizeof(StackType_t)), /* usStackDepth */
			param, /* pvParameters */
			(UBaseType_t)priority, /* uxPriority */
			&taskHandle); /* pxCreatedTask */

	if (status != 0) {
		return NULL; /* 创建失败。 */
	}

	return taskHandle;
}

/**
 * @brief 删除一个任务（旧版 API 包装）。
 *
 * @param[in] task 要删除的任务句柄。
 * @return 0 表示已提交删除；-1 表示句柄为 NULL。
 * @note htTaskDelete() 本身不返回错误码，因此这里只能保证参数合法，
 *       无法报告删除动作是否真正完成。
 */
int htOSTaskDelete(TaskHandle_t task)
{
	if (task == NULL) {
		return -1; /* 参数错误：句柄为空。 */
	}

	htTaskDelete((TaskHandle_t)task);
	return 0; /* 内核删除接口无返回值，按成功处理。 */
}

/**
 * @brief 创建队列（旧版 API 包装）。
 *
 * @param[in] length 队列长度，即可存放的消息条数，至少为 1。
 * @param[in] itemSize 每条消息的字节数。
 * @return 成功返回队列句柄；长度非法、参数溢出或内存不足时返回 NULL。
 */
QueueHandle_t htOSQueueCreate(uint32_t length, uint32_t itemSize)
{
	return htQueueCreate((UBaseType_t)length, (UBaseType_t)itemSize);
}

/**
 * @brief 向队列发送一条消息（旧版 API 包装）。
 *
 * @param[in] queue 目标队列句柄。
 * @param[in] item 待发送数据的指针，拷贝长度由创建队列时的 itemSize 决定。
 * @param[in] timeout 等待空位的超时时间，单位毫秒；UINT32_MAX 表示永久等待。
 * @return 0 表示发送成功；-1 表示参数非法或等待超时。
 * @note 队列满时本函数会阻塞当前任务，直到出现空位或超时。
 * @warning 不能在中断服务程序里调用，ISR 请使用 htQueueSendFromISR()。
 */
int htOSQueueSend(QueueHandle_t queue, const void *item, uint32_t timeout)
{
	if (queue == NULL || item == NULL) {
		return -1; /* 参数错误。 */
	}

	/* 先把毫秒超时换算成滴答超时，内核只认滴答。 */
	TickType_t xTicksToWait = prvMillisecondsToTicks(timeout);

	BaseType_t status = htQueueSend(queue, item, xTicksToWait);
	return (status == htPASS) ? 0 : -1;
}

/**
 * @brief 从队列接收一条消息（旧版 API 包装）。
 *
 * @param[in] queue 源队列句柄。
 * @param[out] buffer 接收缓冲区，容量必须不小于创建队列时的 itemSize。
 * @param[in] timeout 等待消息的超时时间，单位毫秒；UINT32_MAX 表示永久等待。
 * @return 0 表示接收成功；-1 表示参数非法或等待超时。
 * @note 队列为空时本函数会阻塞当前任务，直到有消息或超时。
 * @warning 不能在中断服务程序里调用，ISR 请使用 htQueueReceiveFromISR()。
 */
int htOSQueueReceive(QueueHandle_t queue, void *buffer, uint32_t timeout)
{
	if (queue == NULL || buffer == NULL) {
		return -1; /* 参数错误。 */
	}

	/* 先把毫秒超时换算成滴答超时，内核只认滴答。 */
	TickType_t xTicksToWait = prvMillisecondsToTicks(timeout);

	BaseType_t status = htQueueReceive(queue, buffer, xTicksToWait);
	return (status == htPASS) ? 0 : -1;
}

/**
 * @brief 创建二值信号量（旧版 API 包装）。
 *
 * @return 信号量句柄；内存不足时返回 NULL。
 * @note 初始状态为“不可用”（计数为 0），需要先 give 一次才能 take 成功。
 */
SemaphoreHandle_t htOSSemaphoreCreateBinary(void)
{
	return htSemaphoreCreateBinary();
}

/**
 * @brief 创建互斥锁（旧版 API 包装）。
 *
 * @return 互斥锁句柄；内存不足时返回 NULL。
 * @note 初始状态为“未上锁”（计数为 1），创建后可以直接 take。
 * @note 这是计数上限为 1 的二值互斥锁：没有持有者记录，不支持递归获取，
 *       也不做优先级继承；需要这些特性请用 htSemaphoreCreateRecursiveMutex()。
 */
SemaphoreHandle_t htOSMutexCreate(void)
{
	return htSemaphoreCreateMutex();
}

/**
 * @brief 获取信号量或互斥锁（旧版 API 包装）。
 *
 * @param[in] sem 信号量或互斥锁句柄。
 * @param[in] timeout 等待超时时间，单位毫秒；UINT32_MAX 表示永久等待。
 * @return 0 表示获取成功；-1 表示参数非法或等待超时。
 * @note 计数为 0 时会阻塞当前任务，直到其他任务/中断 give 或超时。
 * @warning 不能在中断服务程序里调用；ISR 中请使用 htSemaphoreTakeFromISR()。
 */
int htOSSemaphoreTake(SemaphoreHandle_t sem, uint32_t timeout)
{
	if (sem == NULL) {
		return -1; /* 参数错误。 */
	}

	/* 先把毫秒超时换算成滴答超时，内核只认滴答。 */
	TickType_t xTicksToWait = prvMillisecondsToTicks(timeout);

	BaseType_t status = htSemaphoreTake(sem, xTicksToWait);
	return (status == htPASS) ? 0 : -1;
}

/**
 * @brief 释放信号量或互斥锁（旧版 API 包装）。
 *
 * @param[in] sem 信号量或互斥锁句柄。
 * @return 0 表示释放成功；-1 表示参数非法或信号量已满。
 * @note 二值信号量/互斥锁的计数上限为 1，重复 give 会失败并返回 -1。
 */
int htOSSemaphoreGive(SemaphoreHandle_t sem)
{
	if (sem == NULL) {
		return -1; /* 参数错误。 */
	}

	BaseType_t status = htSemaphoreGive(sem);
	return (status == htPASS) ? 0 : -1;
}
