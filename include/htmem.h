/**
 * @file htmem.h
 * @brief TLSF（Two-Level Segregated Fit）内存分配器接口。
 *
 * TLSF 是一种实时内存分配算法，分配与释放均为 O(1) 常数时间。主要特点：
 * - 双层分离适配：一级索引（FL）与二级索引（SL）快速定位空闲块；
 * - 位图快速搜索：用两级位图在常数时间内找到非空空闲链表；
 * - 物理块连续：相邻空闲块在释放时可合并，减少外部碎片；
 * - 实时性能：所有操作都不随堆规模增长。
 */

#ifndef HT_MEM_H
#define HT_MEM_H

/** @brief 提供 size_t 定义。 */
#include <stddef.h>
/** @brief 提供 uint32_t 等定宽整数定义。 */
#include <stdint.h>
#include "httypes.h"

/* ------------------------------------------------------------------ */
/* 1. TLSF 算法配置参数                                                 */
/* ------------------------------------------------------------------ */

/**
 * @defgroup tlsf_config TLSF 索引配置
 * @{
 * 算法原理：
 * - 一级索引（FL）按块大小的最高有效位划分，跨度较大；
 * - 二级索引（SL）在一级确定的区间内再细分 16 档，用于精确定位；
 * - 空闲链表总数为 FL_COUNT × SL_COUNT 条。
 * @}
 */

/** @brief 二级索引位数，取 4 表示每个一级区间分 16 档（2^4 = 16）。 */
#define TLSF_SL_INDEX_COUNT_LOG2 4
/** @brief 一级索引上限，支持小于 2^14 字节的块，足以覆盖默认 12KB 堆。 */
#define TLSF_FL_INDEX_MAX 14
/** @brief 二级索引个数，恒为 16。 */
#define TLSF_SL_INDEX_COUNT (1 << TLSF_SL_INDEX_COUNT_LOG2)
/** @brief 一级索引位移，值为 8，即小块与大块的阈值是 2^8 = 256 字节。 */
#define TLSF_FL_INDEX_SHIFT (TLSF_SL_INDEX_COUNT_LOG2 + 3)
/** @brief 一级索引数量，由二级索引位移与一级上限共同决定。 */
#define TLSF_FL_INDEX_COUNT (TLSF_FL_INDEX_MAX - TLSF_FL_INDEX_SHIFT + 1)
/** @brief 小块上限，256 字节；不超过该值的块走线性查找。 */
#define TLSF_SMALL_BLOCK_SIZE (1 << TLSF_FL_INDEX_SHIFT)

/* ------------------------------------------------------------------ */
/* 2. 内存对齐                                                          */
/* ------------------------------------------------------------------ */

/**
 * @brief 块对齐粒度，等于指针大小。
 *
 * ARM Cortex-M 要求 32 位数据四字节对齐，所有分配块都按该值向上对齐，
 * 32 位平台上即为 4 字节。
 */
#define TLSF_ALIGN_SIZE (sizeof(void *))

/* ------------------------------------------------------------------ */
/* 3. 块状态位                                                          */
/* ------------------------------------------------------------------ */

/**
 * @brief 块头 size 字段的低两位为状态标志，高位保存真实块大小。
 * - bit 0：当前块是否空闲；
 * - bit 1：物理上前一个块是否空闲（用于释放时的合并判断）。
 */
/** @brief 当前块空闲标志（bit 0）。 */
#define TLSF_BLOCK_CURR_FREE (1 << 0)
/** @brief 物理上前一块空闲标志（bit 1）。 */
#define TLSF_BLOCK_PREV_FREE (1 << 1)
/** @brief 大小掩码，屏蔽低两位状态后得到纯块大小。 */
#define TLSF_SIZE_MASK (~(TLSF_BLOCK_CURR_FREE | TLSF_BLOCK_PREV_FREE))

/* ------------------------------------------------------------------ */
/* 4. 块头结构                                                          */
/* ------------------------------------------------------------------ */

/**
 * @brief TLSF 内存块头，紧跟用户数据区之前。
 *
 * 每个块（空闲或已分配）都带此头。布局要点：
 * 1. prev_phys_block：物理相邻前块的指针，用于释放时向前合并；
 * 2. size：块大小与状态位打包，可快速取大小、查状态；
 * 3. next_free / prev_free：仅空闲块使用，串成双向空闲链表。
 *
 * 内存布局：[块头][用户数据][下一个块头]...
 */
typedef struct block_header {
	struct block_header *prev_phys_block; /**< 物理上前一个块，用于合并相邻空闲块 */
	size_t size; /**< 块大小，低两位为状态标志 */

	/* 以下两个字段仅在块空闲时有效，用于构成空闲链表 */
	struct block_header *next_free; /**< 空闲链表中下一个块 */
	struct block_header *prev_free; /**< 空闲链表中上一个块 */
} block_header_t;

/** @brief 块头字节数。 */
#define TLSF_BLOCK_HEADER_SIZE (sizeof(block_header_t))
/** @brief 用户数据相对块头的偏移，等于块头大小。 */
#define TLSF_BLOCK_START_OFFSET (sizeof(block_header_t))
/** @brief 堆管理额外开销，为堆首、堆尾两个块头之和。 */
#define TLSF_POOL_OVERHEAD (2 * TLSF_BLOCK_HEADER_SIZE)

/* ------------------------------------------------------------------ */
/* 5. 控制结构                                                          */
/* ------------------------------------------------------------------ */

/**
 * @brief TLSF 控制结构，算法核心状态。
 *
 * 包含三部分：
 * 1. 二维空闲链表数组 blocks[FL][SL]：每个槽位挂一串大小落在同一区间的空闲块；
 * 2. 两级位图：fl_bitmap 标记哪些 FL 非空，sl_bitmap[FL] 标记其下哪些 SL 非空，
 *    借此在 O(1) 时间内定位到非空链表；
 * 3. 统计信息：用于调试与观察内存压力。
 */
typedef struct tlsf_control {
	/* 二维空闲链表：[一级索引][二级索引] -> 空闲块链表头 */
	block_header_t *blocks[TLSF_FL_INDEX_COUNT][TLSF_SL_INDEX_COUNT];

	/* 位图索引 */
	unsigned int fl_bitmap; /**< 一级位图，标记哪些 FL 存在空闲块 */
	unsigned int sl_bitmap[TLSF_FL_INDEX_COUNT]; /**< 二级位图，每个 FL 对应一张 */

	/* 堆统计信息 */
	size_t total_size; /**< 堆总大小（字节） */
	size_t free_size; /**< 当前空闲字节数 */
	size_t min_free_size; /**< 历史最小空闲字节数，用于评估内存压力 */
} tlsf_control_t;

/* ------------------------------------------------------------------ */
/* 6. 公共接口                                                          */
/* ------------------------------------------------------------------ */

/**
 * @brief 初始化 TLSF 内存管理器。
 *
 * 设置静态堆区域与控制结构，建立初始大空闲块与尾部哨兵块。
 * 系统启动时调用一次；重复调用是幂等的，不会重新初始化。
 *
 * @note configTOTAL_HEAP_SIZE 为 0 时本函数为空实现，分配接口均返回 NULL。
 */
void htMemInit(void);

/**
 * @brief 从堆中分配一块内存。
 *
 * @param[in] size 请求字节数。
 * @return 成功返回用户可用指针，失败返回 NULL。
 * @note 实际分配长度向上对齐到 TLSF_ALIGN_SIZE；整个过程在临界区内完成，
 *       可在任意上下文调用。size 为 0 或请求超出可用空间时返回 NULL。
 */
void *htPortMalloc(size_t size);

#if (configUSE_MALLOC_FAILED_HOOK == 1)
/**
 * @brief 内存分配失败钩子。
 *
 * @param[in] xWantedSize 本次申请失败的字节数。
 *
 * @note 弱符号，默认空实现。每次 htPortMalloc() 因为堆里没有足够大的块而返回
 *       NULL 时调用一次；请求长度为 0 或长度溢出算调用错误，不触发。
 * @note 在临界区之外调用，可以安全地做串口输出等动作。
 */
void htMallocFailedHook(size_t xWantedSize);
#endif

/**
 * @brief 释放此前由 htPortMalloc() 分配的块。
 *
 * @param[in] ptr 待释放的内存指针；传 NULL 时直接返回。
 * @note 会向前、向后合并相邻空闲块以减少碎片；传入非本堆内的野指针会被
 *       块头一致性检查拒绝而不是破坏堆。
 */
void htPortFree(void *ptr);

/**
 * @brief 读取当前空闲堆字节数。
 *
 * @return 当前空闲内存总量（字节）。
 */
size_t htGetFreeHeapSize(void);

/**
 * @brief 读取历史最小空闲堆字节数。
 *
 * @return 程序运行至今出现过的最小空闲量（字节）。
 * @note 可用于评估内存使用峰值与发现内存泄漏。
 */
size_t htGetMinimumEverFreeHeapSize(void);

#endif /* HT_MEM_H */
