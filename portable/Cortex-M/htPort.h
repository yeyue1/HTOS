/**
 * @file htPort.h
 * @brief Cortex-M 特定的移植层定义
 */
#ifndef HT_PORT_H
#define HT_PORT_H

#include <stdint.h>



/**
 * 启动第一个任务
 */
void vPortStartFirstTask(void);

/**
 * PendSV中断处理函数
 * 用于任务切换
 */
void PendSV_Handler(void);

/**
 * SVC中断处理函数
 * 用于启动第一个任务
 */
void SVC_Handler(void);

#endif /* HT_PORT_H */
