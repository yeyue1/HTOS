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

## 注意事项

1. **堆栈大小**：确保为任务分配足够的堆栈空间，避免堆栈溢出  
2. **优先级设置**：合理规划任务优先级，避免高优先级任务长时间占用CPU  
3. **临界区保护**：访问共享资源时使用信号量或互斥锁进行保护  
4. **内存使用**：系统使用静态内存池，在`htconfig.h`中的`configTOTAL_HEAP_SIZE`定义总堆大小  
5. **ISR与任务通信**：在中断服务例程中使用FromISR系列函数与任务通信

## 待实现功能

1. 事件组（Event Groups）  
2. 软件定时器（Software Timers）  
3. 任务通知增强功能  
4. 更完善的任务统计和调试功能  
5. 支持Cortex-M4/M0等其他ARM核

## 移植注意事项

htOS依赖于ARM Cortex-M3的PendSV机制实现任务切换。移植到其他平台需要修改以下部分：

1. `htport.c`中的任务上下文切换代码  
2. 系统滴答定时器配置  
3. 中断优先级设置

## 常见问题解决

1. **系统无法启动**：检查是否已创建至少一个任务，以及`htOSStart()`是否被调用  
2. **任务不执行**：检查任务优先级设置和调度器状态  
3. **内存分配失败**：增加`configTOTAL_HEAP_SIZE`或优化内存使用  
4. **信号量操作超时**：检查是否有高优先级任务长时间持有信号量

## 个人申明
代码完全开源，目前还有很多功能待完善，仅作学习用。

希望这个RTOS内核及其 CoreDump 模块能帮助你更方便地调试嵌入式系统问题。
