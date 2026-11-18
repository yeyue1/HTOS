/**
 * @file htdebug.h
 * @brief 调试日志宏。
 *
 * 由 configENABLE_DEBUG_LOG 在编译期决定 HTOS_DEBUG_LOG 是输出日志还是完全消失，
 * 关闭时不产生任何格式化开销。
 */
#ifndef HT_DEBUG_H
#define HT_DEBUG_H

#include "htconfig.h"

/**
 * @brief 调试日志宏，用法与 printf 相同（HTOS_DEBUG_LOG("x=%d\r\n", x)）。
 *
 * configENABLE_DEBUG_LOG 为真时展开为 printf()，需要工程已把 printf 重定向到串口等输出；
 * 为假时展开为 ((void)0)，实参不会被求值，也不生成格式化字符串。
 *
 * @note configENABLE_DEBUG_LOG 目前未在 htconfig.h 中定义。按 C 预处理器规则，未定义标识符
 *       在 #if 中求值为 0，因此默认关闭；要启用需在编译选项或工程配置里自行定义该宏。
 * @warning 打开后 printf 可能阻塞较长时间，不要在中断服务程序或临界区内输出日志。
 */
#if configENABLE_DEBUG_LOG
#include <stdio.h>
#define HTOS_DEBUG_LOG(...) printf(__VA_ARGS__)
#else
#define HTOS_DEBUG_LOG(...) ((void)0)
#endif

#endif /* HT_DEBUG_H */
