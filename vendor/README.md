# vendor/ —— 厂商与架构支持文件（只读）

本目录**只放来自芯片厂商 / Arm 的原始文件，永不修改**。
它存在的唯一目的是让本仓库能真正为 STM32F103C8T6 编译、链接与烧录；
在此之前仓库里没有任何 CMSIS / 器件头 / startup / 链接脚本，导致 9 个内核编译单元只有 1 个能编译。

## 来源

全部从本机既有的一个可工作工程只读复制而来（未联网、未改动内容）：

```
C:\Users\yeyue\Desktop\book\参考代码\openOCD_vscode标准调试+HTOS+栈回溯\
```

## 内容与版本

| 路径 | 内容 | 版本 | 许可证 |
|---|---|---|---|
| `CMSIS/Core/Include/` | ARM CMSIS Core（`core_cm3.h`、`cmsis_gcc.h`、`cmsis_compiler.h`、`cmsis_version.h`） | **CMSIS Core 5.1** | **Apache-2.0**（`CMSIS/LICENSE.txt`） |
| `CMSIS/Device/ST/STM32F1xx/Include/` | ST 器件头（`stm32f1xx.h`、`stm32f103xb.h`、`system_stm32f1xx.h`） | STM32F1xx CMSIS Device | **Apache-2.0**（`.../STM32F1xx/LICENSE.txt`） |
| `CMSIS/Device/ST/STM32F1xx/Source/Templates/system_stm32f1xx.c` | 系统初始化 | 同上 | 同上 |
| `CMSIS/Device/ST/STM32F1xx/Source/Templates/gcc/startup_stm32f103xb.s` | **GCC 版**启动文件（向量表 / Reset_Handler / 弱符号 ISR） | 同上 | 同上 |
| `STM32F1xx_HAL_Driver/` | ST HAL 驱动（Inc 70 + Src 61） | STM32Cube F1 | **BSD-3-Clause**（`STM32F1xx_HAL_Driver/LICENSE.txt`） |
| `linker/STM32F103C8TX_FLASH.ld` | 链接脚本：**64 KB FLASH / 20 KB RAM** | — | ST 模板（随 CubeMX 工程） |

许可证全部为 **Apache-2.0 / BSD-3-Clause**，符合项目红线（仅允许 MIT / Apache-2.0 / BSD）。
**无 GPL / LGPL，无任何依赖 malloc 的库。**

## 硬性规则（改这些文件等于破坏可复现性）

1. `startup_stm32f103xb.s`、`linker/*.ld`、`CMSIS/**` —— **禁止修改**。
2. HAL 只允许被 `board/` 层使用；`kernel/` 与 `portable/` 不得包含 HAL 头
   （当前 `kernel/htos.c`、`kernel/httask.c`、`kernel/htscheduler.c` 仍违规，属待优化的分层缺陷）。
3. 升级 vendor 时整目录替换并更新本文件的版本表，不要零散补文件
   （踩过的坑：逐个补 `stm32f1xx_hal_conf.h` 依赖会连续缺 `Legacy/stm32_hal_legacy.h`、
   `stm32f1xx_hal_exti.h`、`stm32f1xx_hal_dma_ex.h`，最终仍需全量镜像）。

## 与仓库既有约定的一致性

- 用户红线："必须保留 CMSIS 与启动文件" —— 本目录即该载体。
- 用户红线："不修改 startup_stm32f103xb.s、链接脚本、CMSIS 头文件" —— 本目录内文件一律只读。
- 用户红线："所有寄存器操作必须走 CMSIS" —— 有了本目录，`SCB->ICSR`、`NVIC_SetPriority`、
  `SysTick->LOAD` 等 CMSIS 接口才是**可用**的；此前内核被迫硬编码 `0xE000ED04`
  （`kernel/httask.c` 的 `htTaskYield`），就是缺 CMSIS 的直接后果。
