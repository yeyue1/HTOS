/**
 * @file board/retarget.c
 * @brief 本固件所需的最小 newlib（nano.specs）系统调用层。
 *
 * 这里有两个刻意为之的性质：
 *  1. STDOUT 接到轮询式 USART3 控制台（RS485，9600 8N1），因此内核里任何一处
 *     意外的 printf() 会从 USB-RS485 适配器出来，而不是掉进默认桩函数里空转。
 *     本文件自身不含任何 printf() 调用。
 *  2. _sbrk() 永远失败。本固件没有堆：RTOS 使用自己固定的 TLSF 内存池
 *     （configTOTAL_HEAP_SIZE），不允许任何代码绕开调度器去占 RAM，
 *     因此误用的 malloc/calloc/realloc 会返回 NULL，而不是悄悄把堆顶往上挪。
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "board.h"

/* -------------------------------------------------------------------------- */
/* 写：stdout/stderr -> USART3                                                */
/* -------------------------------------------------------------------------- */
/**
 * @brief 把缓冲区的字节送到控制台串口。
 *
 * @param[in] file 文件描述符，未使用（stdout/stderr 都走同一个控制台）。
 * @param[in] ptr  待发送数据。
 * @param[in] len  字节数。
 * @return 实际"接受"的字节数（等于 len）；参数非法时返回 0。
 * @note 返回值必须是 len，否则 newlib 的 printf 会认为自己被截断并反复重试。
 */
int _write(int file, char *ptr, int len)
{
	(void)file;

	if ((ptr == NULL) || (len <= 0)) {
		return 0;
	}

	board_console_write(ptr, (uint32_t)len);

	return len;
}

/* -------------------------------------------------------------------------- */
/* 堆：本固件没有堆。任何分配都必须明确失败，而不是偷走 RAM。                 */
/* -------------------------------------------------------------------------- */
/**
 * @brief 堆增长请求——永远拒绝。
 *
 * @param[in] increment 请求扩展的字节数，未使用。
 * @return 一定返回 (void *)-1，同时把 errno 置为 ENOMEM。
 * @note 这是全局唯一的堆边界实现。返回 -1 而不是 0：返回 0 会被 newlib 当成
 *       "分配成功"，那才是真正危险的行为。
 */
void *_sbrk(ptrdiff_t increment)
{
	(void)increment;

	errno = ENOMEM;
	/* 刻意为之：本固件无堆（只有 RTOS 内存池 + 静态缓冲区）。 */
	return (void *)(intptr_t)-1;
}

/* -------------------------------------------------------------------------- */
/* nano.specs 还要求提供的其余桩函数                                          */
/* -------------------------------------------------------------------------- */
/**
 * @brief 读系统调用桩。
 *
 * @param[in] file 文件描述符，未使用。
 * @param[in] ptr  接收缓冲区，未使用。
 * @param[in] len  请求字节数，未使用。
 * @return 永远返回 0（EOF）。
 * @note 本条控制台没有输入通路（半双工收发器的发送端只接 TX）。
 */
int _read(int file, char *ptr, int len)
{
	(void)file;
	(void)ptr;
	(void)len;

	/* 本条控制台没有输入通路：直接报 EOF。 */
	return 0;
}

/**
 * @brief 关闭文件描述符桩。
 *
 * @param[in] file 文件描述符，未使用。
 * @return 永远返回 -1（没有可关闭的对象）。
 */
int _close(int file)
{
	(void)file;

	return -1;
}

/**
 * @brief 定位系统调用桩。
 *
 * @param[in] file   文件描述符，未使用。
 * @param[in] offset 偏移量，未使用。
 * @param[in] whence 基准位置，未使用。
 * @return 永远返回 0（控制台不是可寻址设备）。
 */
int _lseek(int file, int offset, int whence)
{
	(void)file;
	(void)offset;
	(void)whence;

	return 0;
}

/**
 * @brief 取文件状态桩。
 *
 * @param[in]  file 文件描述符，未使用。
 * @param[out] st   状态结构，只填 st_mode。
 * @return 永远返回 0。
 * @note 把 st_mode 标成字符设备，newlib 才会按"无缓冲"的方式写 stdout。
 */
int _fstat(int file, struct stat *st)
{
	(void)file;

	st->st_mode = S_IFCHR; /* 字符设备 */

	return 0;
}

/**
 * @brief 判断是否为终端桩。
 *
 * @param[in] file 文件描述符，未使用。
 * @return 永远返回 1（当作终端处理，newlib 因此按行/按块刷缓冲）。
 */
int _isatty(int file)
{
	(void)file;

	return 1;
}

/**
 * @brief 发信号桩。
 *
 * @param[in] pid 目标进程号，未使用。
 * @param[in] sig 信号编号，未使用。
 * @return 永远返回 -1，同时把 errno 置为 EINVAL。
 * @note 裸机上没有进程概念，这里只求"失败得干净"。
 */
int _kill(int pid, int sig)
{
	(void)pid;
	(void)sig;

	errno = EINVAL;

	return -1;
}

/**
 * @brief 取进程号桩。
 *
 * @return 永远返回 1（固定值，nano.specs 需要它存在）。
 */
int _getpid(void)
{
	return 1;
}

/**
 * @brief 进程退出桩：无路可走，把 MCU 停在这里。
 *
 * @param[in] status 退出码，未使用。
 * @note nano.specs 并不提供 _exit，但 newlib 的 exit() 会调用它；
 *       不提供的话，任何引用到 exit() 的链接都会失败。
 * @warning 死循环是刻意设计：调试器仍可以暂停内核并查看调用栈。
 */
void _exit(int status)
{
	(void)status;

	for (;;) {
		/* 停在此处；调试器依然可以暂停内核 */
	}
}
