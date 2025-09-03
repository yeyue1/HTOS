<<<<<<< HEAD
#ifndef HT_MEM_H
#define HT_MEM_H

#include <stddef.h>  // 添加此行以提供size_t定义
#include "httypes.h"

/* 内存块结构 */
typedef struct BlockLink
{
    struct BlockLink *pxNextFreeBlock;   /* 指向下一个空闲块 */
    size_t xBlockSize;                  /* 块大小 */
} BlockLink_t;

/* 内存管理函数 */
void htMemInit(void);
void *htPortMalloc(size_t xWantedSize);
void htPortFree(void *pv);
size_t htGetFreeHeapSize(void);
size_t htGetMinimumEverFreeHeapSize(void);

#endif /* HT_MEM_H */
=======
#ifndef HT_MEM_H
#define HT_MEM_H

#include <stddef.h>  // 添加此行以提供size_t定义
#include "httypes.h"

/* 内存块结构 */
typedef struct BlockLink
{
    struct BlockLink *pxNextFreeBlock;   /* 指向下一个空闲块 */
    size_t xBlockSize;                  /* 块大小 */
} BlockLink_t;

/* 内存管理函数 */
void htMemInit(void);
void *htPortMalloc(size_t xWantedSize);
void htPortFree(void *pv);
size_t htGetFreeHeapSize(void);
size_t htGetMinimumEverFreeHeapSize(void);

#endif /* HT_MEM_H */
>>>>>>> 71740f394ac024936657a80886e3630583dd7361
