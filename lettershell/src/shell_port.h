/**
 * @file shell_port.h
 * @brief Letter Shell 移植层头文件 for HTOS
 */

#ifndef SHELL_PORT_H
#define SHELL_PORT_H

#include "shell.h"

/* 函数声明 */
void shellPortInit(void);

/* Shell命令函数声明 */
int shellTestCmd(int argc, char *argv[]);
int shellSystemInfo(int argc, char *argv[]);
int shellLedControl(int state);

#endif /* SHELL_PORT_H */
