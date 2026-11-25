/**
 * @file board/stm32f1xx_it.c
 * @brief 板级层负责的 Cortex-M 异常处理函数。
 *
 * 这里只定义 SysTick 一个处理函数，而它正是整个系统唯一的 tick 来源：
 * 它把节拍转发给 RTOS 的 tick 入口 htSchedulerTickHandler()，除此之外不再
 * 做任何耗时的动作（不 printf、不延时、也不自己写调度逻辑）。
 *
 * PendSV_Handler、SVC_Handler、HardFault_Handler 是刻意不在这里定义的：
 * 它们由 portable/Cortex-M/htPortContext.c 提供，而启动文件
 * （gcc/startup_stm32f103xb.s）已经为每个向量提供了弱符号 Default_Handler
 * 别名，再定义一次会造成符号重复。
 */

#include "stm32f1xx_hal.h"

#include "board.h"
#include "htscheduler.h"

/**
 * @brief SysTick 中断：既是 HAL 的 1 ms 时基，也是唯一的 RTOS 节拍。
 *
 * @note HAL_IncTick() 让 HAL_GetTick() 继续有效，阻塞式 USART3 发送正是用它
 *       来限定超时时间。这只是 HAL 的记账，不是第二个调度节拍。
 * @warning 本函数处于中断上下文：不许 printf、不许 HAL_Delay()、不许任何
 *          可能阻塞的调用，所以函数体只有两行。
 */
void SysTick_Handler(void)
{
	HAL_IncTick();
	htSchedulerTickHandler();
}

#if (BOARD_CONSOLE_ASYNC_TX == 1)
/**
 * @brief USART3 中断服务函数：仅在启用 BOARD_CONSOLE_ASYNC_TX 时参与编译。
 */
void USART3_IRQHandler(void)
{
	board_console_irq_handler();
}
#endif
