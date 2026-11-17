/**
  ******************************************************************************
  * @file    board/stm32f1xx_it.c
  * @brief   Cortex-M exception handlers owned by the board layer.
  *
  * ONLY the SysTick handler is defined here, and it is the single tick source of
  * the system: it forwards to the RTOS tick entry point htSchedulerTickHandler()
  * and does nothing else that costs time (no printf, no delay, no scheduler
  * logic of its own).
  *
  * PendSV_Handler, SVC_Handler and HardFault_Handler are intentionally NOT
  * defined here: portable/Cortex-M/htPortContext.c owns them, and the startup
  * file (gcc/startup_stm32f103xb.s) already provides weak Default_Handler
  * aliases for every vector, so redefining them would duplicate a symbol.
  ******************************************************************************
  */

#include "stm32f1xx_hal.h"

#include "htscheduler.h"

/**
  * @brief  SysTick interrupt: 1 ms time base for the HAL and the only RTOS tick.
  * @note   HAL_IncTick() keeps HAL_GetTick() alive, which is what the blocking
  *         USART1 transmit uses for its timeout bound.  It is HAL bookkeeping,
  *         not a second scheduler tick.
  */
void SysTick_Handler(void)
{
	HAL_IncTick();
	htSchedulerTickHandler();
}
