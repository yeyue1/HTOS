/**
 * @file shell_port.c
 * @brief Letter Shell 移植层实现 for HTOS
 */

#include "shell.h"
#include "shell_cfg_user.h"
#include "shell_port.h"
#include "bsp_usart.h" // 包含串口相关定义
#include "htos.h"
#include "httask.h"
#include "usart.h" // 包含HAL相关定义
#include "stm32f1xx.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* 基础类型定义 - 临时解决编译问题 */
typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;
typedef void *TaskHandle_t;

/* Shell对象 */
Shell shell;
static char shellBuffer[512];

/* Shell接收相关变量 */
static uint8_t shellRxProcessed = 0; // 已处理的字节数

/* 外部变量声明 */
extern TickType_t xTickCount;
extern htTCB_t *pxCurrentTCB;
extern UBaseType_t uxCurrentNumberOfTasks;
extern uint8_t aRxBuffer2[RXBUFFERSIZE]; // 串口2接收缓冲区
extern uint8_t RX_len2; // 串口2接收长度
extern UART_HandleTypeDef huart2; // 串口2句柄

/**
 * @brief shell读字符函数
 * @param data 读取的字符缓冲区
 * @param len 缓冲区长度
 * @return 读取到的字符个数
 */
signed short shellRead(char *data, unsigned short len)
{
	/* 如果没有数据直接返回0 */
	if (RX_len2 == 0) {
		return 0;
	}

	/* 可读字节数 */
	uint16_t avail = RX_len2;
	uint16_t toRead = (avail > len) ? len : avail;

	/* 复制数据 */
	for (uint16_t i = 0; i < toRead; i++) {
		data[i] = (char)aRxBuffer2[i];
	}

	/* 标记数据已被读取；由你的 bsp/usart 在空闲中断中填充 RX_len2，因此这里清零表示已消费 */
	RX_len2 = 0;

	/* 如果使用 DMA，需要在 bsp_usart 中确保重新启动接收（通常在 IDLE 回调里已处理） */

	return (signed short)toRead;
}

/**
 * @brief shell写字符函数
 * @param data 要写入的字符缓冲区
 * @param len 要写入的字符个数
 * @return 写入的字符个数
 */
signed short shellWrite(char *data, unsigned short len)
{
	if (&huart2 != NULL) {
		RS485_Send;
		HAL_Delay(1); // 10ms延时
		/* 阻塞发送，超时时间可根据波特率调整 */
		HAL_UART_Transmit(&huart2, (uint8_t *)data, len, len * 10 + 100);
			RS485_Receive;
	}
	return (signed short)len;
}

/**
 * @brief shell任务函数
 * @param argument 参数
 */
void shellTask(void *argument)
{
	(void)argument;
	char data;
	/* 无限循环 */
	for (;;) {
		// 读取一个字符，如果有则处理
		if (shell.read(&data, 1) == 1) {
			shellHandler(&shell, data);
		}
		htTaskDelay(10); // 10ms延时
	}
}

/**
 * @brief shell初始化
 */
void shellPortInit(void)
{
	/* shell初始化 */
	shell.write = shellWrite;
	shell.read = shellRead;

	shellInit(&shell, shellBuffer, sizeof(shellBuffer));

	/* 创建shell任务 */
	TaskHandle_t shellTaskHandle;
	shellTaskHandle = htOSTaskCreate("Shell", shellTask, NULL, 4096, HT_HIGH_TASK);

	/* 显示欢迎信息 */
	printf("\r\n========================================\r\n");
	printf("      Welcome to HTOS Letter Shell     \r\n");
	printf("========================================\r\n");
	printf("Build: %s %s\r\n", __DATE__, __TIME__);
	printf("Type 'help' for available commands\r\n");
	printf("\r\n");
}

/* 测试命令实现 */
int shellTestCmd(int argc, char *argv[])
{
	printf("Hello Letter Shell on HTOS!\r\n");
	printf("Arguments: %d\r\n", argc);
	for (int i = 0; i < argc; i++) {
		printf("  [%d]: %s\r\n", i, argv[i]);
	}
	return 0;
}

/* 系统信息命令 */
int shellSystemInfo(int argc, char *argv[])
{
	(void)argc;
	(void)argv;
	printf("=== HTOS System Information ===\r\n");
	printf("System Tick: %lu\r\n", (unsigned long)xTickCount);
	printf("Current Task: %s\r\n", pxCurrentTCB ? pxCurrentTCB->pcTaskName : "Unknown");
	printf("Task Count: %lu\r\n", (unsigned long)uxCurrentNumberOfTasks);

	// 显示内存信息（如果有实现的话）
	printf("Memory Status: Available\r\n");

	return 0;
}

/* LED控制命令 */
int shellLedControl(int state)
{
	if (state) {
		printf("LED ON (GPIO PC13 LOW)\r\n");
		// HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_RESET); // 点亮LED
	} else {
		printf("LED OFF (GPIO PC13 HIGH)\r\n");
		// HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET); // 熄灭LED
	}
	return 0;
}

/* 导出命令 */
SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN), test, shellTestCmd, test command);
SHELL_EXPORT_CMD(
		SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN), sysinfo, shellSystemInfo, show system info);
SHELL_EXPORT_CMD(
		SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_FUNC), led, shellLedControl, led control\r\nled[0 | 1]);
