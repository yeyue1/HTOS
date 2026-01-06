# htOS - 轻量级Cortex-M3 RTOS内核
 
## 项目简介

htOS是一个专为ARM Cortex-M3微控制器设计的轻量级实时操作系统内核，实现了基本的任务调度、信号量和消息队列等功能。该RTOS遵循经典的优先级抢占式调度模型，适合资源受限的嵌入式系统应用。

## 已实现功能

- **任务管理**：创建、删除、挂起、恢复任务
- **调度器**：优先级抢占式调度
- **内存管理**：静态内存池分配
- **列表管理**：用于维护任务状态和队列
- **消息队列**：支持任务间数据交换
- **信号量**：支持二值信号量、计数信号量和互斥量
- **临界区保护**：中断禁用/使能机制
- **任务延时**：精确的时间延迟功能

## 文件结构

```
HTOS/
├── kernel/       - 内核源代码
│   ├── htlist.c      - 列表管理实现
│   ├── htmem.c       - 内存管理实现
│   ├── htos.c        - 操作系统核心功能
│   ├── htqueue.c     - 队列实现
│   ├── htscheduler.c - 调度器实现
│   ├── htsemaphore.c - 信号量实现
│   ├── httask.c      - 任务管理实现
│   └── htutils.c     - 工具函数
├── include/      - 头文件
│   ├── htconfig.h    - 系统配置
│   ├── htlist.h      - 列表API定义
│   ├── htmem.h       - 内存管理API
│   ├── htos.h        - 系统API定义
│   ├── htqueue.h     - 队列API定义
│   ├── htscheduler.h - 调度器API定义
│   ├── htsemaphore.h - 信号量API定义
│   ├── httask.h      - 任务API定义
│   ├── httypes.h     - 类型定义
│   └── htutils.h     - 工具函数API
└── Trace/
    └── coredump/   - CoreDump 模块
        ├── inc/
        ├── src/
        ├── arch/
        └── osal/
```

## 使用方法

### 系统初始化

```c
#include "htos.h"

int main(void)
{
    // 初始化系统硬件
    
    // 初始化htOS
    htOSInit();
    
    // 创建任务
    
    // 启动调度器
    htOSStart();
    
    // 此处代码永远不会被执行
    return 0;
}
```

### 创建任务

```c
// 任务函数定义
void vTask1(void *pvParameters)
{
    while(1)
    {
        // 任务执行代码
        
        // 延时
        htTaskDelay(htTimeToTicks(100)); // 延时100毫秒
    }
}

// 创建任务
TaskHandle_t xTask1Handle;
htTaskCreate(vTask1,             // 任务函数
             "Task1",           // 任务名称
             128,               // 堆栈深度（单位：字）
             NULL,              // 任务参数
             2,                 // 优先级
             &xTask1Handle);    // 任务句柄
```

### 使用队列

```c
// 创建队列
QueueHandle_t xQueue;
xQueue = htQueueCreate(10, sizeof(uint32_t)); // 10个元素，每个元素4字节

// 发送数据
uint32_t value = 100;
htQueueSend(xQueue, &value, 0); // 发送数据，不等待

// 接收数据
uint32_t receivedValue;
if(htQueueReceive(xQueue, &receivedValue, htTimeToTicks(100)) == htPASS) {
    // 成功接收数据
}
```

### 使用信号量

```c
// 创建二值信号量
SemaphoreHandle_t xSemaphore;
xSemaphore = htSemaphoreCreateBinary();

// 获取信号量
if(htSemaphoreTake(xSemaphore, htTimeToTicks(100)) == htPASS) {
    // 成功获取信号量
    
    // 处理受保护的资源
    
    // 释放信号量
    htSemaphoreGive(xSemaphore);
}
```

## 重要API参考

### 任务管理

- `htTaskCreate()` - 创建新任务
- `htTaskDelete()` - 删除任务
- `htTaskDelay()` - 任务延时
- `htTaskYield()` - 任务让出CPU

### 队列管理

- `htQueueCreate()` - 创建队列
- `htQueueSend()` - 发送数据到队列
- `htQueueReceive()` - 从队列接收数据
- `htQueueDelete()` - 删除队列

### 信号量

- `htSemaphoreCreateBinary()` - 创建二值信号量
- `htSemaphoreCreateCounting()` - 创建计数信号量
- `htSemaphoreCreateMutex()` - 创建互斥量
- `htSemaphoreTake()` - 获取信号量
- `htSemaphoreGive()` - 释放信号量

### 内存管理

- `htPortMalloc()` - 分配内存
- `htPortFree()` - 释放内存

## htmem 使用说明

`htmem` 实现基于 TLSF（Two‑Level Segregated Fit），提供实时性友好的动态内存分配接口。以下为使用要点、API 示例与调优建议。

主要 API
- `void htMemInit(void);`              // 可选：显式初始化堆（首次分配会自动初始化）
- `void *htPortMalloc(size_t size);`   // 分配内存（按 TLSF_ALIGN_SIZE 对齐），失败返回 NULL
- `void htPortFree(void *ptr);`        // 释放内存（自动合并相邻空闲块）
- `size_t htGetFreeHeapSize(void);`    // 当前可用堆大小（字节）
- `size_t htGetMinimumEverFreeHeapSize(void);` // 运行期间出现过的最小空闲量（用于评估内存峰值）
- `size_t htGetTotalHeapSize(void);`   // 配置的总堆大小（configTOTAL_HEAP_SIZE）

初始化与示例
- 推荐在系统启动时调用 `htMemInit()`，但不调用也会在第一次 `htPortMalloc` 时自动初始化。
示例：
```c
// 在 main 或系统初始化阶段
htMemInit();

// 分配与释放
void *p = htPortMalloc(256);
if (p == NULL) {
    // 处理分配失败：记录/降级/重试
}
htPortFree(p);
```

对齐与有效负载
- 分配大小会向上对齐到 `TLSF_ALIGN_SIZE`（默认 4 字节）。
- 分配请求会额外包含块头开销（`TLSF_BLOCK_HEADER_SIZE`），因此应考虑实际可用字节。

线程安全与中断
- 当前实现通过禁用中断保护临界区（适合单核 Cortex‑M）。
- 在 ISR 内尽量避免调用 `htPortMalloc` / `htPortFree`；若必须，请确保短时调用并检查失败。更推荐在中断中使用固定大小内存池或事先分配的缓冲区。

大块分配与限制
- 堆总大小由 `configTOTAL_HEAP_SIZE` 决定（`htconfig.h`）。若有任务栈或大缓冲区需求（例如 2KB+），请确保堆足够大或改为静态/链接时分配。
- 若需要频繁大块分配，建议：
  - 提高 `configTOTAL_HEAP_SIZE`；
  - 或对大对象使用专用“回退大块分配器”或静态保留区域。

运行时监控与调试
- 获取当前剩余与历史最小剩余：
```c
size_t free_now = htGetFreeHeapSize();
size_t min_free = htGetMinimumEverFreeHeapSize();
```
- 在内存紧张时记录 `min_free` 以调整堆大小或优化分配策略。
- 可在调试版本中增加断言/日志检查分配失败点，便于定位内存泄漏。

最佳实践
- 任务栈优先静态分配或在创建任务时一次性分配并尽量避免频繁动态分配。
- 对于固定尺寸对象（消息、buffer），优先使用固定大小内存池（pool/slab），性能更稳定且无碎片。
- 在资源受限场景，将 TLSF 用作通用分配器，同时为高频短寿命对象配置专用池，达到性能与利用率折中。

配置项
- `configTOTAL_HEAP_SIZE`（在 `include/htconfig.h` 中定义）控制堆总大小。根据任务栈、队列、缓存等最大同时需求估算并设置合理值。

常见问题与处理
- 分配失败但总内存看似足够：检查碎片（`free_now` 可能小于任意连续空闲块），考虑调整策略或使用大块保留区。
- 非确定性延迟问题：避免在实时临界路径频繁分配，或改用固定池 / TLSF 参数调优以减小最坏延时。

示例：在任务创建中安全使用 htmem
```c
void TaskInit(void)
{
    // 为任务创建必要的缓冲区（优先静态或在初始化阶段一次分配）
    uint8_t *buf = htPortMalloc(512);
    if (!buf) {
        // 记录并降级功能，避免在运行中重复尝试大量分配
    }
    // 使用后在适当时机释放
    // htPortFree(buf);
}
```

## Trace / CoreDump 集成（MCoreDump）

项目包含 Trace/coredump 模块，用于在故障时生成 ELF 格式的 coredump（可串口输出、保存到内存或文件系统）。

位置：Trace/coredump/

推荐将该模块集成到 STM32F1 项目以便在异常或断言时保留调试信息。

主要文件结构（在 Trace/coredump/ 下）：
```
inc/    - coredump.h, mcd_cfg.h, mcd_elf_define.h
src/    - coredump.c, faultdump.c, mcd_arm.c
arch/   - armv7m.c, registers.h, mcd_arch_interface.h
osal/   - htos.c
mcd_example.c - 使用示例
```

集成要点
- 在 Keil/MDK 项目中加入 src/、arch/、osal/ 中的源文件并在头文件路径中加入 inc/、arch/、src/。
- 在 main 中初始化：调用 mcd_htos_init()。
- 若需复位后保留 coredump，修改链接脚本添加 .noinit (NOLOAD) 段或使用等效的 Keil 设置。

快速测试命令（示例）
```c
mcd_test_htos("COREDUMP"); // 立即生成coredump并输出
mcd_test_htos("ASSERT");   // 触发断言
mcd_test_htos("FAULT");    // 触发硬件错误
mcd_test_htos("MEMORY");   // 保存到内存
```

输出模式
- MCD_OUTPUT_SERIAL：串口十六进制输出
- MCD_OUTPUT_MEMORY：保存到内存缓冲区（可配置为 NoInit）
- MCD_OUTPUT_FILESYSTEM：保存到文件系统（需支持）

API 概览
```c
void mcd_htos_init(void);
int  mcd_faultdump(mcd_output_mode_t output_mode);
mcd_bool_t mcd_check_memory_coredump(void);
void mcd_print_memoryinfo(void);
```

配置建议（mcd_cfg.h）
- MCD_MEMORY_SIZE: 建议 4KB (STM32F103 RAM 限制)
- MCD_FPU_SUPPORT: 0（F103 无 FPU）
- PKG_USING_MCOREDUMP_ARCH_ARMV7M: 1

调试与分析
- 可使用 arm-none-eabi-gdb 加载 firmware.elf 和 coredump.elf 进行分析
- 使用十六进制编辑器或 Python 脚本解析 ELF 数据
- 输出示例会包含 ELF 魔数、程序头、栈和寄存器信息，并带 CRC32 校验

更多细节请查看 Trace/coredump/README.md

# letter shell 3.x（已移植到 HTOS）

本仓库中的 Letter Shell（3.x）已被移植到 HTOS 上，提供一个在 HTOS 上运行的嵌入式交互 shell。该文档说明了在 HTOS 中的集成要点、配置建议和常见注意事项。

## 关键变化（HTOS 移植）
- 新增移植层：lettershell/src/shell_port.c / shell_port.h，封装串口读写、任务创建与系统信息命令。
- 在 HTOS 上初始化调用：shellPortInit()（该函数会创建 shell 任务并注册读写回调）。
- 依赖 HTOS 的符号与 BSP：htOS 计时（xTickCount）、任务创建（htOSTaskCreate）、串口缓冲（aRxBuffer2、RX_len2、huart2）、RS485 宏等需在工程中提供。
- 导出命令：test、sysinfo、led（已在 shell_port.c 中注册）。

## 快速集成步骤（HTOS / Keil / GCC）
1. 将文件加入工程
   - 添加 lettershell/src/*.c、lettershell/src/*.h 到工程编译列表（或 CMake / Keil 项目）。
   - 确保 include 路径包含 lettershell/src（或对应路径）。
 
2. 提供或确认以下符号（shell_port.c 依赖）
   - 串口接收缓冲与长度：extern uint8_t aRxBuffer2[]; extern uint8_t RX_len2;
   - 串口句柄：extern UART_HandleTypeDef huart2;
   - HTOS 计时：extern TickType_t xTickCount;
   - 任务创建接口：htOSTaskCreate(...)
   - RS485 控制宏（若使用 RS485）或替换为普通串口发送/接收逻辑。

3. （可选）创建并配置 shell_cfg_user.h 覆盖默认配置（见示例），确保 SHELL_USING_CMD_EXPORT = 1 以启用宏式命令导出。

## 个人申明
代码完全开源，目前还有很多功能待完善，仅作学习用。

希望这个RTOS内核能帮助你更方便地调试嵌入式系统问题。
