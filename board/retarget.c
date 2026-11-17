/**
  ******************************************************************************
  * @file    board/retarget.c
  * @brief   Minimal newlib (nano.specs) system-call layer for this firmware.
  *
  * Two deliberate properties:
  *  1. STDOUT goes to the polled USART1 console, so a stray printf() from the
  *     kernel lands on COM7 instead of spinning in a default stub.  There is no
  *     printf() call in this file.
  *  2. _sbrk() ALWAYS fails.  This firmware has no heap: the RTOS uses its own
  *     fixed TLSF pool (configTOTAL_HEAP_SIZE) and nothing is allowed to take
  *     RAM behind the scheduler's back, so an accidental malloc/calloc/realloc
  *     returns NULL instead of silently growing the heap.
  ******************************************************************************
  */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "board.h"

/* -------------------------------------------------------------------------- */
/* Write: stdout/stderr -> USART1                                             */
/* -------------------------------------------------------------------------- */
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
/* Heap: there is none.  Any allocation must fail loudly, not steal RAM.      */
/* -------------------------------------------------------------------------- */
void *_sbrk(ptrdiff_t increment)
{
	(void)increment;

	errno = ENOMEM;
	/* Deliberate: this firmware is heap-free (RTOS pool + static buffers). */
	return (void *)(intptr_t)-1;
}

/* -------------------------------------------------------------------------- */
/* Remaining stubs required by nano.specs                                     */
/* -------------------------------------------------------------------------- */
int _read(int file, char *ptr, int len)
{
	(void)file;
	(void)ptr;
	(void)len;

	/* No input path on this console: report EOF. */
	return 0;
}

int _close(int file)
{
	(void)file;

	return -1;
}

int _lseek(int file, int offset, int whence)
{
	(void)file;
	(void)offset;
	(void)whence;

	return 0;
}

int _fstat(int file, struct stat *st)
{
	(void)file;

	st->st_mode = S_IFCHR; /* character device */

	return 0;
}

int _isatty(int file)
{
	(void)file;

	return 1;
}

int _kill(int pid, int sig)
{
	(void)pid;
	(void)sig;

	errno = EINVAL;

	return -1;
}

int _getpid(void)
{
	return 1;
}

/**
  * @brief  Terminate the process - there is nowhere to go, so park the MCU.
  * @note   nano.specs does not provide _exit, yet newlib's exit() calls it, so
  *         it must exist or every link that touches exit() fails.
  */
void _exit(int status)
{
	(void)status;

	for (;;) {
		/* stay here; a debugger can still halt the core */
	}
}
