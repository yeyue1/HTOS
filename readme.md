# HTOS

[![HTOS quality gates](https://github.com/yeyue1/HTOS/actions/workflows/main.yaml/badge.svg)](https://github.com/yeyue1/HTOS/actions/workflows/main.yaml)

HTOS 是一个面向 ARM Cortex-M3 的轻量级学习型 RTOS 内核。它提供优先级抢占调度、同优先级时间片、任务延时、任务通知、队列、信号量、静态对象创建和 TLSF 堆分配器，并带有可在 Windows/Linux 上运行的内核回归测试。

> 定位：用于学习、实验和二次开发，不是经过功能安全认证的商业 RTOS。上产品前必须完成目标 MCU 编译、实机压力测试和最坏时序测量。

## 已实现能力

### 任务与调度

- 支持动态创建任务，也可用 `htTaskCreateStatic()` 由调用方提供 TCB 与任务栈，避免运行期堆分配。
- 优先级抢占和同优先级轮转。
- `htTaskDelay()` 相对延时，支持 32 位 tick 回绕。
- `htTaskDelayUntil()` 绝对周期延时，避免周期任务逐次漂移；错过本周期截止点时返回 `htFAIL`。
- 任务优先级查询/修改、挂起/恢复、ISR 恢复、状态/名称/当前句柄查询。
- 每个任务带一个 32 位直接通知槽：支持置位、递增、覆盖写、禁止覆盖写、等待和计数式获取，并提供 ISR 发送接口。
- 当前任务自删除时由空闲任务延后回收 TCB 和栈，避免在正在使用的栈上释放自身。

### 队列与同步

- FIFO 队列：发送、接收、查看、重置和删除。
- 任务上下文支持 0 tick、有限超时和永久等待；有限等待接入调度器延时链表。
- ISR 发送/接收接口可返回是否唤醒更高优先级任务。
- `htQueueSpacesAvailable()` 查询剩余容量。
- `htQueueOverwrite()` 用于长度为 1 的数据队列，保留最新值，适合传感器快照和状态发布。
- `htQueueCreateStatic()` 支持调用方提供队列控制块与数据区；二值/计数信号量和普通互斥令牌也可静态创建。
- 二值信号量、计数信号量和有上限的二值互斥令牌。
- 递归互斥量跟踪持有者和递归深度，并对发生阻塞的高优先级任务执行优先级继承。

### 内存与诊断

- 12 KiB 默认 TLSF 堆，按目标指针宽度对齐。
- 分配大小溢出检查、非法指针防护、重复释放防护和相邻空闲块合并。
- 释放前校验块的**物理前驱一致性**：`prev_phys_block` 必须满足 `prev + size(prev) == block`（或该块本身就是池内首块）。落在堆内的野指针（任务栈、数据缓冲区里的随机字节）几乎不可能满足该不变式，因此在进入空闲链表之前就被拒绝，不会把仍在使用的内存当作空闲块发出去。
- `htGetFreeHeapSize()` 和 `htGetMinimumEverFreeHeapSize()` 用于堆使用量观测。
- 新建任务栈使用 `0xA5A5A5A5` 涂色，`htGetTaskStackHighWaterMark()` 返回历史最小剩余栈，单位为 word。
- 可选 MCoreDump 和 Letter Shell 目录保留在 `Trace/` 和 `lettershell/`。

### 临界区与中断

- 临界区用 PRIMASK 全屏蔽实现（不是 BASEPRI 分级屏蔽），`uxCriticalNesting` 记录嵌套深度，归零时才恢复中断。
- `htEnterCritical()` **先关中断、再递增嵌套计数**。反序会留下一个可抢占窗口：计数已加、PRIMASK 仍为 0 时若被 SysTick 抢占，换出的上下文会多带一层嵌套，返回后 `htExitCritical()` 只能把计数减到 1，永远走不到恢复中断的分支，PRIMASK 永久置位而系统**静默死锁**（不触发任何 fault）。
- 因此临界区必须短小，且不得在临界区内调用可能阻塞的 API。

## 配置开关

`include/htconfig.h` 中的宏并非全部生效。当前**已实现**：`configUSE_PREEMPTION`、
`configUSE_TIME_SLICING`、`configMAX_PRIORITIES`、`configTOTAL_HEAP_SIZE`、
`configMINIMAL_STACK_SIZE`、`configUSE_TASK_NOTIFICATIONS`、
`configUSE_RECURSIVE_MUTEXES`、`configSUPPORT_STATIC_ALLOCATION`、`configUSE_IDLE_HOOK`、
`configCHECK_FOR_STACK_OVERFLOW`。

**尚未实现**的开关会在编译期直接 `#error` 中止，而不是静默忽略：`configUSE_QUEUE_SETS`、
`configGENERATE_RUN_TIME_STATS`、`configUSE_STATS_FORMATTING_FUNCTIONS`、
`configUSE_TICK_HOOK`、`configUSE_MALLOC_FAILED_HOOK`。

## 快速上手

```c
#include "htos.h"
#include "htutils.h"

static void SensorTask(void *argument)
{
	(void)argument;
	TickType_t last_wake = xTickCount;

	for (;;) {
		/* 读取传感器、发送数据等 */
		(void)htTaskDelayUntil(&last_wake, htTimeToTicks(10U));
	}
}

int main(void)
{
	htOSInit();

	TaskHandle_t sensor = NULL;
	if (htTaskCreate(SensorTask, "sensor", 256U, NULL, HT_NORMAL_TASK, &sensor) != htPASS) {
		/* 堆空间不足或参数无效 */
		for (;;) {}
	}

	htOSStart();
	for (;;) {}
}
```

任务栈深度的单位是 `StackType_t` word；Cortex-M3 上 256 word 通常是 1024 字节。

### 只保留最新数据

```c
QueueHandle_t latest_sample = htQueueCreate(1U, sizeof(uint32_t));
uint32_t sample = 123U;

if (latest_sample != NULL) {
	(void)htQueueOverwrite(latest_sample, &sample);
}
```

### 查看栈余量

```c
UBaseType_t remaining_words = htGetTaskStackHighWaterMark(sensor);
if (remaining_words < 32U) {
	/* 记录风险，并根据最坏路径增加栈预算 */
}
```

### 栈溢出检测

打开 `configCHECK_FOR_STACK_OVERFLOW` 后，内核在每次任务切换时检查被换下任务的栈底 4 个字是否仍为填充值 `0xA5A5A5A5`：

```c
void htStackOverflowHook(TaskHandle_t xTask, const char *pcTaskName)
{
	/* 记录任务名并停机；继续运行只会写坏相邻内存 */
	(void)xTask;
	board_console_write(pcTaskName);
	for (;;) {
	}
}
```

该钩子默认是弱符号的死循环，不覆盖也能安全停机。代价是每次切换多 4 次比较（本工程 text +76 B、bss 不变）。只查栈底 4 个字意味着它能发现“写穿栈边界”，但发现不了恰好落在栈内的越界写——后者仍需调试器或静态分析。

实测三个任务的栈余量都很充裕（blink 22/128、logger 69/192、idle 23/128 字）。故障注入（128 字栈里放 512 B 局部数组）时，该钩子在实板上准确报出 `task=hog` 并停机。

### 用任务通知代替单元素队列

任务通知直接存放在目标任务的 TCB 中，适合事件位、轻量计数器和“一对一”唤醒：

```c
/* 生产者：合并多个事件位。 */
(void)htTaskNotify(worker, 0x05U, HT_NOTIFY_SET_BITS);

/* worker 任务：等待通知，并在读取后清除所有事件位。 */
uint32_t events = 0U;
if (htTaskNotifyWait(0U, UINT32_MAX, &events, htBLOCKED_INDEFINITELY) == htPASS) {
	/* 处理 events */
}
```

`htTaskNotifyGive()` / `htTaskNotifyTake()` 提供计数信号量式语义；ISR 中使用 `htTaskNotifyFromISR()` 或 `htTaskNotifyGiveFromISR()`，并检查 `pxHigherPriorityTaskWoken`。

### 静态创建任务与队列

静态对象不会调用 `htPortMalloc()`，其控制块和存储区必须在对象整个生命周期内保持有效：

```c
static htTCB_t worker_tcb;
static StackType_t worker_stack[configMINIMAL_STACK_SIZE];
static htQUEUE_t sample_queue_buffer;
static uint8_t sample_queue_storage[4U * sizeof(uint32_t)];

TaskHandle_t worker = htTaskCreateStatic(WorkerTask, "worker", configMINIMAL_STACK_SIZE, NULL,
		HT_NORMAL_TASK, worker_stack, &worker_tcb);
QueueHandle_t samples = htQueueCreateStatic(
		4U, sizeof(uint32_t), sample_queue_storage, &sample_queue_buffer);
```

### 完全去掉堆（纯静态分配）

把 `configTOTAL_HEAP_SIZE` 设为 `0`，整个 TLSF 池连同 12 KB 的 `g_tlsf_heap` 一起被裁掉（相对默认 12 KB 堆的生产固件：bss −12224 B、Flash −1464 B）；`htPortMalloc()` 恒返回 NULL，动态创建接口全部失败，只能用上面的静态接口。内核自己的空闲任务在该配置下改用编译期静态栈，因此系统仍可调度。实板验证：静态队列 + 两个静态任务连续运行 28 s，51/51 行完整上报，`bad=0`、`free_heap=0`。

## 主机测试

需要 Python 3 和 GCC 或 Clang。

```bash
python tests/run_tests.py --build --cc gcc
python tests/run_tests.py --build --cc clang
python tests/run_tests.py --clean
```

也可在 `tests/` 目录使用 `make test`。当前主机套件包含 4175 个断言，覆盖列表、TLSF 堆、队列/信号量、任务通知、静态对象、递归互斥量、有限超时、tick 回绕、任务状态转换、队列覆盖、栈高水位、栈溢出哨兵，以及分配器边界（最大块、完全耗尽、混合尺寸耗尽、对齐、荒谬尺寸、非法释放、伪造块头、碎片合并、随机 churn、峰值水位）。队列一侧还覆盖了 `*FromISR` 唤醒更高优先级等待者的返回值、重置与查看不改变数据、删除时动态存储被释放；链表一侧覆盖了带哨兵环的遍历辅助接口；通知一侧覆盖了置位/递增/禁止覆盖写等五种动作。

统一构建入口在 `codex_build.toml`：

```bash
python <embedded-build-check>/scripts/build_router.py validate --config codex_build.toml
python <embedded-build-check>/scripts/build_router.py build --config codex_build.toml --profile host_tests_ai_guard
```

## CI/CD

`.github/workflows/main.yaml` 在每次推送到 `main`/`master` 和 Pull Request 时执行：

- Linux GCC：`-Wall -Wextra -Wpedantic -Werror`。
- Linux Clang：AddressSanitizer + UndefinedBehaviorSanitizer。
- Windows Clang：验证测试入口的跨平台性。
- clang-format、clang-tidy 和 cppcheck：静态质量门禁。
- Cortex-M 边界语法编译：覆盖系统封装、任务、调度器和工具代码。

`tests/stubs/` 中的 CMSIS 头文件只用于编译门禁，不模拟中断、寄存器或上下文切换。

## 目录结构

```text
HTOS/
├── include/                 公开 API 与配置
├── kernel/                  任务、调度、队列、同步与 TLSF
├── portable/Cortex-M/       Cortex-M3 上下文切换端口
├── tests/                   跨平台主机回归测试
├── Trace/coredump/          可选 MCoreDump 集成
├── lettershell/             可选 Letter Shell 集成
├── codex_build.toml         统一构建 profile
└── .github/workflows/       GitHub Actions 质量门禁
```

## 已知限制与后续方向

- 仓库没有提供完整的 STM32F1 HAL、启动文件、链接脚本和板级工程；CI 不能代替真实固件链接与上板测试。
- `portable/Cortex-M/htPortContext.c` 已改为 GCC/Thumb 内联汇编，可直接由 `arm-none-eabi-gcc` 构建；CMSIS 核心头文件通过 `HT_PORT_CORE_HEADER` 覆盖，SRAM 栈地址检查默认关闭，启用时用 `HT_PORT_VALIDATE_STACK`、`HT_PORT_SRAM_START` 和 `HT_PORT_SRAM_END` 按链接脚本配置。
- 普通互斥量仍是有上限的二值令牌，不跟踪持有者；递归互斥量已覆盖单锁的持有者、递归深度、超时与优先级继承，但多把互斥量嵌套持有时的优先级回退尚未建模。
- 静态分配目前覆盖任务、通用队列、二值/计数信号量和普通互斥令牌；递归互斥量仍通过动态创建接口构造。
- 软件定时器、事件组和看门狗服务尚未实现。软件定时器应在专用服务任务、命令队列和回调上下文约束明确后再加入。
- `htGetCPUUsage()`、`htGetIdleRunTimePercent()` 和 `htGetRunTimeCounter()` 暂未接入硬件运行时计数器。
- 栈高水位是涂色扫描的历史估计；`configCHECK_FOR_STACK_OVERFLOW` 只能发现写穿栈边界的溢出（只比较栈底 4 个字），栈内的越界写仍需调试器或静态分析；STM32F103 无 MPU/PSPLIM，不能用硬件做栈与内存隔离。
- 已在 STM32F103C8T6（Cortex-M3，72 MHz，64 KB Flash / 20 KB SRAM）实板上完成三轮压测：
  第一轮 60 秒混合负载（队列、互斥量与优先级继承、计数信号量、递归互斥量、任务通知、堆分配回收、
  运行期建删任务），451 万次迭代、218 万次断言零失败；第二轮 180 秒覆盖 tick 回绕、
  ISR 上下文 API、超时精度、挂起/恢复、`htTaskDelayUntil` 周期性与队列边界，在 1 kHz 定时器中断
  持续抢占下 24 万次迭代、120 万次断言零失败，空闲堆恒定、`uxCriticalNesting` 归零；
  第三轮 60 秒全 API 遍历，把 `include/` 下 102 个公开符号分成 11 组顺序自检，同时叠加 12 个并发
  任务与 TIM2 100 Hz 中断里的全部 `*FromISR` 接口，163 万次断言零失败，任务建删 churn 期间堆占用
  回到基线，`CFSR` / `HFSR` 全零。压测固件依赖板级工程，未随内核仓库发布。
- 板级控制台是阻塞轮询发送，低优先级写者在大负载下会被长时间抢占，因此发送超时必须按最坏行长
  留足余量（`BOARD_CONSOLE_TX_TIMEOUT_MS`）。超时的分块不能从头部重发——
  `HAL_UART_Transmit()` 不报告已发出的字节数，重发会重复前缀；只有未开始发送的 `HAL_BUSY`
  可以安全重试。真正的高吞吐日志应改用中断/DMA 环形缓冲。

## License

见 [LICENSE](LICENSE)。
