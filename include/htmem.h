/**
 * @file htmem.h
 * @brief TLSF (Two-Level Segregated Fit) 内存分配器头文件
 * 
 * TLSF是一个高效的实时内存分配算法，提供O(1)的分配和释放性能。
 * 主要特点：
 * - 双层分离适配：使用一级(FL)和二级(SL)索引快速定位空闲块
 * - 位图快速搜索：使用位图加速空闲块查找
 * - 物理块连续性：支持块合并以减少碎片
 * - 实时性能：所有操作都是常数时间复杂度
 */

#ifndef HT_MEM_H
#define HT_MEM_H

#include <stddef.h>  // 提供size_t定义
#include <stdint.h>  // 提供uint32_t等定义
#include "httypes.h"

/**
 * TLSF算法配置参数
 * 
 * 算法原理：
 * - 一级索引(FL)：基于块大小的最高有效位，范围较大
 * - 二级索引(SL)：在FL确定的范围内进一步细分，提供精确定位
 * - 总的空闲列表数量 = FL_COUNT × SL_COUNT
 */
#define TLSF_SL_INDEX_COUNT_LOG2    5    /* 二级索引位数 (2^5=32个二级索引) */
#define TLSF_FL_INDEX_MAX           30   /* 最大一级索引值 (支持最大2^30字节) */
#define TLSF_SL_INDEX_COUNT         (1 << TLSF_SL_INDEX_COUNT_LOG2)  /* 32个二级索引 */
#define TLSF_FL_INDEX_SHIFT         (TLSF_SL_INDEX_COUNT_LOG2 + 3)   /* 8，小块阈值为2^8=256字节 */
#define TLSF_FL_INDEX_COUNT         (TLSF_FL_INDEX_MAX - TLSF_FL_INDEX_SHIFT + 1)  /* 一级索引数量 */
#define TLSF_SMALL_BLOCK_SIZE       (1 << TLSF_FL_INDEX_SHIFT)      /* 256字节，小块与大块的分界 */

/**
 * 内存对齐配置
 * 
 * ARM Cortex-M处理器要求：
 * - 32位数据必须4字节对齐
 * - 所有分配的内存块都按TLSF_ALIGN_SIZE对齐
 */
#define TLSF_ALIGN_SIZE_LOG2        2    /* 对齐位数：2^2 = 4字节对齐 */
#define TLSF_ALIGN_SIZE             (1 << TLSF_ALIGN_SIZE_LOG2)  /* 4字节对齐 */

/**
 * 块状态位定义
 * 
 * 在块的size字段中，低两位用作状态标志：
 * - bit 0: 当前块是否为空闲状态
 * - bit 1: 物理上前一个块是否为空闲状态（用于合并优化）
 * - 高位：存储实际的块大小
 */
#define TLSF_BLOCK_CURR_FREE        (1 << 0)  /* 当前块空闲标志 */
#define TLSF_BLOCK_PREV_FREE        (1 << 1)  /* 前块空闲标志 */
#define TLSF_SIZE_MASK              (~(TLSF_BLOCK_CURR_FREE | TLSF_BLOCK_PREV_FREE))  /* 大小掩码 */

/**
 * TLSF内存块头结构
 * 
 * 每个内存块（无论空闲还是已分配）都有此头部结构。
 * 布局设计考虑：
 * 1. prev_phys_block：物理相邻块的反向指针，用于块合并
 * 2. size：块大小+状态位，支持快速大小查询和状态检查
 * 3. next_free/prev_free：仅空闲块使用，构成双向链表
 * 
 * 内存布局：
 * [block_header][用户数据区域][下一个block_header]...
 */
typedef struct block_header
{
    struct block_header* prev_phys_block;  /* 物理上的前一个块指针（用于合并） */
    size_t size;                          /* 块大小（含状态位） */
    
    /* 以下两个字段仅在块为空闲状态时有效，构成空闲链表 */
    struct block_header* next_free;       /* 空闲链表中的下一个块 */
    struct block_header* prev_free;       /* 空闲链表中的前一个块 */
} block_header_t;

/* 块头大小和偏移计算 */
#define TLSF_BLOCK_HEADER_SIZE      (sizeof(block_header_t))          /* 块头大小 */
#define TLSF_BLOCK_START_OFFSET     (sizeof(block_header_t))          /* 用户数据偏移 */
#define TLSF_POOL_OVERHEAD          (2 * TLSF_BLOCK_HEADER_SIZE)      /* 堆管理开销（首尾块） */

/**
 * TLSF控制结构 - 算法核心数据结构
 * 
 * 这是TLSF算法的核心，包含：
 * 1. 二维空闲链表数组：blocks[FL][SL] 
 *    - FL(First Level)：一级索引，基于块大小范围
 *    - SL(Second Level)：二级索引，在FL范围内细分
 *    - 每个[FL][SL]位置存储相同大小范围的空闲块链表头
 * 
 * 2. 两级位图索引：
 *    - fl_bitmap：一级位图，标记哪些FL有空闲块
 *    - sl_bitmap[FL]：二级位图数组，标记每个FL下哪些SL有空闲块
 *    - 位图支持O(1)时间找到非空的空闲链表
 * 
 * 3. 统计信息：用于调试和监控内存使用情况
 */
typedef struct tlsf_control
{
    /* 二维空闲链表：[一级索引][二级索引] -> 空闲块链表头 */
    block_header_t* blocks[TLSF_FL_INDEX_COUNT][TLSF_SL_INDEX_COUNT];
    
    /* 位图索引系统 */
    unsigned int fl_bitmap;                    /* 一级位图：标记哪些FL有空闲块 */
    unsigned int sl_bitmap[TLSF_FL_INDEX_COUNT]; /* 二级位图：每个FL对应的SL位图 */
    
    /* 堆统计信息 */
    size_t total_size;     /* 堆总大小 */
    size_t free_size;      /* 当前空闲大小 */
    size_t min_free_size;  /* 历史最小空闲大小（用于评估内存压力） */
} tlsf_control_t;

/**
 * TLSF内存分配器公共接口
 * 
 * 设计原则：
 * - 所有函数都提供实时性能保证（O(1)时间复杂度）
 * - 线程安全：通过中断禁用保护临界区
 * - 兼容标准malloc/free接口，便于移植现有代码
 */

/**
 * @brief 初始化TLSF内存管理系统
 * @note 系统启动时调用一次，设置堆区域和控制结构
 */
void htMemInit(void);

/**
 * @brief 分配指定大小的内存块
 * @param size 请求的内存大小（字节）
 * @return 成功返回内存指针，失败返回NULL
 * @note 实际分配大小会向上对齐到TLSF_ALIGN_SIZE边界
 */
void *htPortMalloc(size_t size);

/**
 * @brief 释放之前分配的内存块
 * @param ptr 要释放的内存指针
 * @note 会自动合并相邻空闲块以减少碎片
 */
void htPortFree(void *ptr);

/**
 * @brief 获取当前可用堆大小
 * @return 当前空闲内存总量（字节）
 */
size_t htGetFreeHeapSize(void);

/**
 * @brief 获取历史最小可用堆大小
 * @return 程序运行期间出现过的最小空闲内存量
 * @note 用于评估内存使用峰值和检测内存泄漏
 */
size_t htGetMinimumEverFreeHeapSize(void);

<<<<<<< HEAD
<<<<<<< HEAD
=======
=======
>>>>>>> main/main


static inline int tlsf_block_within_heap(const block_header_t* block);

<<<<<<< HEAD
>>>>>>> db6a41e (change)
=======
>>>>>>> main/main
#endif /* HT_MEM_H */
