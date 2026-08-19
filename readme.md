# HTOS

[![HTOS quality gates](https://github.com/yeyue1/HTOS/actions/workflows/main.yaml/badge.svg)](https://github.com/yeyue1/HTOS/actions/workflows/main.yaml)

HTOS 是一个面向 ARM Cortex-M3 的轻量级学习型 RTOS 内核。它提供优先级抢占调度、同优先级时间片、任务延时、队列、信号量和 TLSF 堆分配器，并带有可在 Windows/Linux 上运行的内核回归测试。

> 定位：用于学习、实验和二次开发，不是经过功能安全认证的商业 RTOS。上产品前必须完成目标 MCU 编译、实机压力测试和最坏时序测量。

## 已实现能力

### 任务与调度

- 动态创建任务，支持名称、参数、栈深度和 32 级优先级。
- 优先级抢占和同优先级轮转。
- `htTaskDelay()` 相对延时，支持 32 位 tick 回绕。
- `htTaskDelayUntil()` 绝对周期延时，避免周期任务逐次漂移；错过本周期截止点时返回 `htFAIL`。
- 任务优先级查询/修改、挂起/恢复、ISR 恢复、状态/名称/当前句柄查询。
- 当前任务自删除时由空闲任务延后回收 TCB 和栈，避免在正在使用的栈上释放自身。

### 队列与同步

- FIFO 队列：发送、接收、查看、重置和删除。
- 任务上下文支持 0 tick、有限超时和永久等待；有限等待接入调度器延时链表。
- ISR 发送/接收接口可返回是否唤醒更高优先级任务。
- `htQueueSpacesAvailable()` 查询剩余容量。
- `htQueueOverwrite()` 用于长度为 1 的数据队列，保留最新值，适合传感器快照和状态发布。
- 二值信号量、计数信号量和有上限的二值互斥令牌。

### 内存与诊断

- 12 KiB 默认 TLSF 堆，按目标指针宽度对齐。
- 分配大小溢出检查、非法指针防护、重复释放防护和相邻空闲块合并。
- `htGetFreeHeapSize()` 和 `htGetMinimumEverFreeHeapSize()` 用于堆使用量观测。
- 新建任务栈使用 `0xA5A5A5A5` 涂色，`htGetTaskStackHighWaterMark()` 返回历史最小剩余栈，单位为 word。
- 可选 MCoreDump 和 Letter Shell 目录保留在 `Trace/` 和 `lettershell/`。

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

## 主机测试

需要 Python 3 和 GCC 或 Clang。

```bash
python tests/run_tests.py --build --cc gcc
python tests/run_tests.py --build --cc clang
python tests/run_tests.py --clean
```

也可在 `tests/` 目录使用 `make test`。测试覆盖列表、TLSF 堆、队列/信号量、有限超时、tick 回绕、任务状态转换、队列覆盖和栈高水位。

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
- `portable/Cortex-M/htPortContext.c` 仍为 Keil 风格汇编，并带有固定 SRAM 地址范围检查；迁移到其他 MCU 前必须参数化。
- 递归互斥量仍是实验路径，未纳入当前主机回归套件；实时代码优先使用普通互斥令牌。
- 任务通知、软件定时器、事件组和看门狗服务尚未实现；`configUSE_TASK_NOTIFICATIONS` 默认关闭。
- `htGetCPUUsage()`、`htGetIdleRunTimePercent()` 和 `htGetRunTimeCounter()` 暂未接入硬件运行时计数器。
- 栈高水位是涂色扫描的历史估计；量产版还应增加 MPU/PSPLIM（如芯片支持）、栈溢出钩子和故障注入测试。

## License

见 [LICENSE](LICENSE)。
