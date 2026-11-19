/**
 * @file host_cmsis.c
 * @brief 主机测试侧 CMSIS 外设替身的存储定义。
 *
 * tests/stubs/core_cm3.h 把 DWT 与 CoreDebug 指向这两块存储。之所以需要单独的
 * 定义文件，是因为用例要写 DWT->CYCCNT 构造时间差、被测代码要读它，两边必须
 * 看到同一份数据；若把存储写成头文件里的 static，每个编译单元都会各持一份，
 * 用例写的时间就传不到被测代码里。
 */
#include "core_cm3.h"

DWT_Type htHostDwt; /**< 主机侧 DWT 替身，CYCCNT 就是一块普通内存。 */
CoreDebug_Type htHostCoreDebug; /**< 主机侧 CoreDebug 替身，只有 DEMCR 一位有意义。 */
