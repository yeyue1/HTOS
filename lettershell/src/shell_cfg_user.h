#ifndef SHELL_CFG_USER_H
#define SHELL_CFG_USER_H

// HTOS项目中的Letter Shell配置

// 启用命令导出方式
#ifndef SHELL_USING_CMD_EXPORT
#define SHELL_USING_CMD_EXPORT 1
#endif

// 任务运行方式 - 配合HTOS任务系统
#ifndef SHELL_TASK_WHILE
#define SHELL_TASK_WHILE 1
#endif

// 基础配置
#ifndef SHELL_PARAMETER_MAX_NUMBER
#define SHELL_PARAMETER_MAX_NUMBER 8
#endif

#ifndef SHELL_HISTORY_MAX_NUMBER
#define SHELL_HISTORY_MAX_NUMBER 5
#endif

#ifndef SHELL_PRINT_BUFFER
#define SHELL_PRINT_BUFFER 256
#endif

#ifndef SHELL_SCAN_BUFFER
#define SHELL_SCAN_BUFFER 256
#endif

// 回车配置
#ifndef SHELL_ENTER_CR
#define SHELL_ENTER_CR 1
#endif

#ifndef SHELL_ENTER_LF
#define SHELL_ENTER_LF 1
#endif

// 功能启用
#ifndef SHELL_HELP_LIST_USER
#define SHELL_HELP_LIST_USER 1
#endif

#ifndef SHELL_HELP_LIST_VAR
#define SHELL_HELP_LIST_VAR 1
#endif

#ifndef SHELL_HELP_LIST_KEY
#define SHELL_HELP_LIST_KEY 1
#endif

#ifndef SHELL_SHOW_INFO
#define SHELL_SHOW_INFO 1
#endif

#ifndef SHELL_QUICK_HELP
#define SHELL_QUICK_HELP 1
#endif

// 内存管理 - 使用HTOS的内存管理
#ifndef SHELL_MALLOC
#define SHELL_MALLOC(size) htPortMalloc(size)
#endif

#ifndef SHELL_FREE
#define SHELL_FREE(obj) htPortFree(obj)
#endif

// 系统Tick - 使用HTOS的tick
#ifndef SHELL_GET_TICK
#define SHELL_GET_TICK() xTickCount
#endif

// 默认用户
#ifndef SHELL_DEFAULT_USER
#define SHELL_DEFAULT_USER "root"
#endif

#ifndef SHELL_DEFAULT_USER_PASSWORD
#define SHELL_DEFAULT_USER_PASSWORD ""
#endif

// 支持函数签名解析
#ifndef SHELL_USING_FUNC_SIGNATURE
#define SHELL_USING_FUNC_SIGNATURE 1
#endif

#endif // SHELL_CFG_USER_H
