/**
 * @file httrace.h
 * @brief 内核诊断输出的唯一出口：两个由应用实现的弱符号钩子。
 *
 * 内核自己一行格式化代码都不留。所有诊断信息（启动失败、空闲统计）都交给应用层
 * 决定怎么落地：接串口、攒环形缓冲、还是直接丢掉。这样内核不再依赖 libc 的
 * printf/vfprintf，newlib 的格式化机制不会被链接进固件。
 *
 * 两个钩子都是弱符号，内核自带空实现（kernel/htos.c）。应用不实现它们时，
 * 所有诊断调用都退化成一次空函数调用，开销可忽略；主机构建与单元测试也不需要
 * 额外桩函数。
 *
 * @warning 两个钩子可能在临界区或中断上下文里被调用，实现里不许阻塞、不许延时、
 *          不许申请内存，也不许调用任何调度接口。
 */
#ifndef HT_TRACE_H
#define HT_TRACE_H

#include <stdint.h>

/**
 * @brief 输出一条静态诊断信息。
 *
 * @param[in] pcMessage 以 NUL 结尾的静态字符串，生命周期覆盖整个程序运行期，
 *                      实现方可以直接保存指针而不必复制内容。
 * @note 该钩子只传静态文本，不传格式串和可变参数，因此不会把格式化代码拉进内核。
 */
void htTrace(const char *pcMessage);

/**
 * @brief 输出一条“标签 + 一个无符号整数”的诊断信息。
 *
 * 内核里需要带数值的诊断只有空闲任务的 tick 统计，所以钩子刻意只留一个整数槽位；
 * 需要更复杂的格式时由实现方自己组合，内核不承担格式化职责。
 *
 * @param[in] pcTag   以 NUL 结尾的静态标签字符串。
 * @param[in] ulValue 与标签一起输出的数值。
 */
void htTraceValue(const char *pcTag, uint32_t ulValue);

#endif /* HT_TRACE_H */
