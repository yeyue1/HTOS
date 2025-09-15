# HTOS CoreDump 移植文档

本文档描述了如何将MCoreDump功能移植到基于HTOS的STM32F103项目中。

## 文件结构

```
HTOS/Trace/coredump/
├── inc/                     # 头文件目录
│   ├── coredump.h          # 主要的coredump接口
│   ├── mcd_cfg.h           # 配置文件
│   └── mcd_elf_define.h    # ELF格式定义
├── src/                     # 源文件目录
│   ├── coredump.c          # 核心coredump实现
│   ├── faultdump.c         # 错误转储实现
│   ├── mcd_arm.c           # ARM架构实现
│   └── mcd_arm_define.h    # ARM架构定义
├── arch/                    # 架构相关文件
│   ├── armv7m.c            # ARM Cortex-M3架构实现
│   ├── registers.h         # 寄存器定义
│   └── mcd_arch_interface.h # 架构接口
├── osal/                    # 操作系统抽象层
│   └── htos.c              # HTOS适配层
└── mcd_example.c           # 使用示例
```

## 集成步骤

### 1. 在Keil项目中添加源文件

在MDK-ARM项目中添加以下文件到编译列表：

**Core Files:**
- `HTOS/Trace/coredump/src/coredump.c`
- `HTOS/Trace/coredump/src/faultdump.c`
- `HTOS/Trace/coredump/src/mcd_arm.c`

**Architecture Files:**
- `HTOS/Trace/coredump/arch/armv7m.c`

**OS Abstraction:**
- `HTOS/Trace/coredump/osal/htos.c`

**Example (可选):**
- `HTOS/Trace/coredump/mcd_example.c`

### 2. 添加头文件路径

在Keil项目的C/C++设置中添加以下头文件路径：
- `HTOS/Trace/coredump/inc`
- `HTOS/Trace/coredump/arch`
- `HTOS/Trace/coredump/src`

### 3. 在main.c中初始化

```c
#include "HTOS/Trace/coredump/inc/coredump.h"

int main(void)
{
    /* HAL初始化 */
    HAL_Init();
    SystemClock_Config();
    
    /* 初始化coredump系统 */
    mcd_htos_init();
    
    /* 其他初始化代码 */
    // ...
    
    /* 启动HTOS调度器 */
    // htosStart();
}
```

### 4. 配置内存布局（可选）

为了让coredump数据在复位后保持，可以在链接脚本中添加：

```ld
/* 在RAM区域添加NoInit段 */
.noinit (NOLOAD) :
{
    . = ALIGN(4);
    *(.noinit*)
    . = ALIGN(4);
} > RAM
```

## 使用方法

### 1. 基本测试

```c
/* 生成即时coredump */
mcd_test_htos("COREDUMP");

/* 触发断言失败 */
mcd_test_htos("ASSERT");

/* 触发硬件错误 */
mcd_test_htos("FAULT");

/* 保存到内存 */
mcd_test_htos("MEMORY");
```

### 2. 输出模式

- **MCD_OUTPUT_SERIAL**: 通过串口输出十六进制数据
- **MCD_OUTPUT_MEMORY**: 保存到内存缓冲区（复位后保留）
- **MCD_OUTPUT_FILESYSTEM**: 保存到文件系统（需要文件系统支持）

### 3. API接口

```c
/* 初始化系统 */
void mcd_htos_init(void);

/* 生成coredump */
int mcd_faultdump(mcd_output_mode_t output_mode);

/* 检查内存中的coredump */
mcd_bool_t mcd_check_memory_coredump(void);

/* 打印内存信息 */
void mcd_print_memoryinfo(void);
```

## 配置选项

### 内存配置 (mcd_cfg.h)

```c
/* 内存缓冲区大小 (适合STM32F103的有限RAM) */
#define MCD_MEMORY_SIZE                (4 * 1024)    /* 4KB */

/* FPU支持 (STM32F103无FPU) */
#define MCD_FPU_SUPPORT                0

/* 架构支持 */
#define PKG_USING_MCOREDUMP_ARCH_ARMV7M    1
```

## 故障诊断

### 1. 编译错误

- 确保所有头文件路径正确添加
- 确保所有源文件都在项目中
- 检查CMSIS头文件版本兼容性

### 2. 运行时问题

- 检查内存配置是否正确
- 确保串口正常工作
- 验证HTOS任务结构体定义

### 3. 输出问题

- 串口输出: 检查printf重定向
- 内存输出: 检查内存区域配置
- 数据完整性: 验证CRC32校验

## 注意事项

1. **内存限制**: STM32F103 RAM有限，建议设置较小的缓冲区
2. **实时性**: 在中断中生成coredump可能影响系统实时性
3. **堆栈**: 确保有足够的堆栈空间进行coredump操作
4. **复位保持**: 使用NoInit段需要链接脚本支持

## 示例输出

### 成功的CoreDump输出示例

```
Generating coredump...
=== CoreDump Start ===
7F454C46010101610000000000000000
04002800010000000000000034000000
00000000000000003400200002000000
...
=== CoreDump End ===
Total bytes: 1308, CRC32: 0xCBD2544A
```

这个输出显示：
- **ELF格式**: 标准的7F454C46魔数开头
- **ARM架构**: 04002800表示ARM Cortex-M
- **数据完整性**: 1308字节，CRC32校验通过
- **内存快照**: 包含栈、寄存器和关键内存区域

### 输出数据解析

1. **ELF头部** (前64字节):
   - 魔数: 7F 45 4C 46 (ELF)
   - 类别: 32位 ARM
   - 字节序: 小端
   - 版本: ELF版本1

2. **程序头部**:
   - PT_NOTE段: 包含寄存器状态
   - PT_LOAD段: 包含内存数据

3. **内存映像**:
   - 栈空间快照
   - 程序计数器和寄存器
   - 异常状态信息

### 内存信息输出
```
=== HTOS CoreDump Memory Info ===
Buffer size: 4096 bytes
Valid coredump found: 1308 bytes
CRC32: 0xCBD2544A
Use 'mcd_dump_memory' command to output coredump.
```

### 调试分析工具

生成的coredump可以用以下工具分析：

1. **GDB分析**:
   ```bash
   arm-none-eabi-gdb firmware.elf
   (gdb) target core coredump.elf
   (gdb) bt          # 查看调用栈
   (gdb) info registers  # 查看寄存器
   ```

2. **十六进制分析**:
   - 使用HxD、010 Editor等工具
   - 按ELF格式解析二进制数据

3. **自动化脚本**:
   - Python脚本解析ELF格式
   - 提取关键信息如PC、SP、LR等

## 扩展功能

1. **多线程支持**: 修改HTOS适配层以支持多线程dump
2. **文件系统**: 添加SD卡或Flash文件系统支持
3. **压缩**: 添加数据压缩以节省存储空间
4. **网络传输**: 通过WiFi/Ethernet传输coredump数据
