/**
 * @file htmem.c
 * @brief TLSF (Two-Level Segregated Fit) 内存分配器实现
 * 
 * 实现原理：
 * 1. 分离适配策略：将空闲块按大小分类存储在不同链表中
 * 2. 双层索引系统：使用FL+SL实现O(1)的空闲块定位
 * 3. 立即合并策略：释放时立即合并相邻空闲块
 * 4. 位图加速查找：使用位操作快速找到合适的空闲块
 * 
 * 性能特点：
 * - 分配/释放：O(1)时间复杂度
 * - 内存开销：每块16字节头部 + 控制结构
 * - 碎片控制：通过立即合并和最佳适配减少外部碎片
 * 
 * @author HTOS Team
 * @date 2025
 */
#include <string.h>
#include "htmem.h"
#include "htconfig.h"

/**
 * TLSF算法辅助宏定义
 * 用于类型转换和数值比较，增强代码可读性
 */
#define tlsf_cast(t, exp)   ((t)(exp))     /* 安全类型转换 */
#define tlsf_min(a, b)      ((a) < (b) ? (a) : (b))  /* 取最小值 */
#define tlsf_max(a, b)      ((a) > (b) ? (a) : (b))  /* 取最大值 */

/**
 * 位操作辅助函数 - ARM Cortex-M兼容版本
 * 
 * 这些函数替代GCC内建函数，确保在不同编译器下的兼容性。
 * 在TLSF算法中用于：
 * 1. 计算块大小对应的FL索引（基于最高有效位）
 * 2. 在位图中快速找到第一个置位的位（空闲链表查找）
 */

/**
 * @brief 查找最高有效位位置 (Find Last Set)
 * @param word 输入的32位无符号整数
 * @return 最高有效位的位置（0-31），如果输入为0则返回0
 * 
 * 算法原理：使用二分查找法，逐步缩小范围
 * 时间复杂度：O(log n) = O(5) 常数时间
 */
static inline int tlsf_fls(unsigned int word)
{
    if (word == 0) return 0;
    
    int bit = 32;
    /* 二分查找：每次排除一半的位 */
    if (!(word & 0xFFFF0000)) { word <<= 16; bit -= 16; }  /* 高16位为0，查找低16位 */
    if (!(word & 0xFF000000)) { word <<= 8; bit -= 8; }    /* 高8位为0，查找低8位 */
    if (!(word & 0xF0000000)) { word <<= 4; bit -= 4; }    /* 高4位为0，查找低4位 */
    if (!(word & 0xC0000000)) { word <<= 2; bit -= 2; }    /* 高2位为0，查找低2位 */
    if (!(word & 0x80000000)) { word <<= 1; bit -= 1; }    /* 最高位为0，查找次高位 */
    
    return bit - 1;  /* 转换为从0开始的索引 */
}

/**
 * @brief 查找最低有效位位置 (Find First Set)
 * @param word 输入的32位无符号整数
 * @return 最低有效位的位置（0-31），如果输入为0则返回-1
 * 
 * 算法原理：类似tlsf_fls，但查找方向相反
 * 用于位图搜索，快速找到第一个非空的空闲链表
 */
static inline int tlsf_ffs(unsigned int word)
{
    if (word == 0) return -1;
    
    int bit = 0;
    /* 二分查找：从低位向高位查找 */
    if (!(word & 0x0000FFFF)) { word >>= 16; bit += 16; }  /* 低16位为0，查找高16位 */
    if (!(word & 0x000000FF)) { word >>= 8; bit += 8; }    /* 低8位为0，查找高8位 */
    if (!(word & 0x0000000F)) { word >>= 4; bit += 4; }    /* 低4位为0，查找高4位 */
    if (!(word & 0x00000003)) { word >>= 2; bit += 2; }    /* 低2位为0，查找高2位 */
    if (!(word & 0x00000001)) { word >>= 1; bit += 1; }    /* 最低位为0，查找次低位 */
    
    return bit;
}

/**
 * 内存对齐辅助函数
 * 
 * ARM处理器访问未对齐的数据会导致性能下降或异常，
 * 因此所有内存分配都必须满足对齐要求。
 */

/**
 * @brief 向上对齐到指定边界
 * @param size 要对齐的大小
 * @param align 对齐边界（必须是2的幂）
 * @return 对齐后的大小
 * 
 * 算法：size + (align-1) 然后清除低位
 * 例如：对齐到4字节边界，align=4，掩码=~3=...11111100
 */
static inline size_t tlsf_align_up(size_t size, size_t align)
{
    return (size + (align - 1)) & ~(align - 1);
}

/**
 * @brief 向下对齐到指定边界
 * @param size 要对齐的大小  
 * @param align 对齐边界（必须是2的幂）
 * @return 对齐后的大小
 * 
 * 算法：直接清除低位
 */
static inline size_t tlsf_align_down(size_t size, size_t align)
{
    return size - (size & (align - 1));
}

/**
 * 内存块操作函数集
 * 
 * 这些函数实现对内存块头部信息的操作，包括：
 * 1. 大小信息的提取和设置（考虑状态位）
 * 2. 空闲/已用状态的查询和设置
 * 3. 块之间的物理连接关系
 * 4. 指针和块头之间的转换
 * 
 * 设计要点：
 * - size字段同时存储块大小和状态位
 * - 使用位操作高效处理状态标志
 * - 所有操作都是内联函数，保证性能
 */

/**
 * @brief 获取块的实际大小
 * @param block 内存块指针
 * @return 块大小（不包含状态位）
 */
static inline size_t tlsf_block_size(const block_header_t* block)
{
    return block->size & TLSF_SIZE_MASK;  /* 屏蔽掉状态位，只保留大小 */
}

/**
 * @brief 设置块大小（保持状态位不变）
 * @param block 内存块指针
 * @param size 新的块大小
 */
static inline void tlsf_block_set_size(block_header_t* block, size_t size)
{
    const size_t oldsize = block->size;
    /* 保留原有状态位，更新大小部分 */
    block->size = size | (oldsize & (TLSF_BLOCK_CURR_FREE | TLSF_BLOCK_PREV_FREE));
}

/**
 * @brief 检查是否为结束标志块
 * @param block 内存块指针
 * @return 1表示是结束块，0表示普通块
 * 
 * 结束块大小为0，用于标记堆的结束边界
 */
static inline int tlsf_block_is_last(const block_header_t* block)
{
    return tlsf_block_size(block) == 0;
}

/**
 * @brief 检查块是否为空闲状态
 * @param block 内存块指针
 * @return 1表示空闲，0表示已分配
 */
static inline int tlsf_block_is_free(const block_header_t* block)
{
    return tlsf_cast(int, block->size & TLSF_BLOCK_CURR_FREE);
}

/**
 * @brief 将块标记为空闲状态
 * @param block 内存块指针
 */
static inline void tlsf_block_set_free(block_header_t* block)
{
    block->size |= TLSF_BLOCK_CURR_FREE;
}

/**
 * @brief 将块标记为已分配状态
 * @param block 内存块指针
 */
static inline void tlsf_block_set_used(block_header_t* block)
{
    block->size &= ~TLSF_BLOCK_CURR_FREE;
}

/**
 * @brief 检查前一个物理块是否为空闲状态
 * @param block 内存块指针
 * @return 1表示前块空闲，0表示前块已分配
 * 
 * 此信息用于合并优化：释放时可快速判断是否需要向前合并
 */
static inline int tlsf_block_is_prev_free(const block_header_t* block)
{
    if (block->prev_phys_block)
    {
        return tlsf_cast(int, block->size & TLSF_BLOCK_PREV_FREE);
    }
    return 0;
}

/**
 * @brief 设置前一个物理块为空闲状态标志
 * @param block 内存块指针
 */
static inline void tlsf_block_set_prev_free(block_header_t* block)
{
    if (block->prev_phys_block)
    {
        block->size |= TLSF_BLOCK_PREV_FREE;
    }
}

/**
 * @brief 设置前一个物理块为已分配状态标志
 * @param block 内存块指针
 */
static inline void tlsf_block_set_prev_used(block_header_t* block)
{
    block->size &= ~TLSF_BLOCK_PREV_FREE;
}

/**
 * 指针转换和块导航函数
 * 
 * 这些函数处理用户指针与内存块头之间的转换，
 * 以及内存块之间的物理位置关系。
 */

/**
 * @brief 从用户指针获取对应的块头指针
 * @param ptr 用户数据指针
 * @return 对应的块头指针
 * 
 * 原理：用户指针 = 块头指针 + TLSF_BLOCK_START_OFFSET
 */
static inline block_header_t* tlsf_block_from_ptr(const void* ptr)
{
    return tlsf_cast(block_header_t*, 
        tlsf_cast(char*, ptr) - TLSF_BLOCK_START_OFFSET);
}

/**
 * @brief 从块头指针获取对应的用户指针
 * @param block 块头指针
 * @return 用户数据指针
 */
static inline void* tlsf_block_to_ptr(const block_header_t* block)
{
    return tlsf_cast(void*, 
        tlsf_cast(char*, block) + TLSF_BLOCK_START_OFFSET);
}

/**
 * @brief 根据偏移量计算块头位置
 * @param ptr 基准指针
 * @param size 偏移量（字节）
 * @return 偏移后的块头指针
 */
static inline block_header_t* tlsf_offset_to_block(const void* ptr, size_t size)
{
    return tlsf_cast(block_header_t*, tlsf_cast(char*, ptr) + size);
}

/**
 * @brief 获取物理上的下一个内存块
 * @param block 当前块指针
 * @return 下一个块的指针
 * 
 * 原理：下一块位置 = 当前块 + 当前块大小
 */
static inline block_header_t* tlsf_block_next(const block_header_t* block)
{
    return tlsf_offset_to_block(block, tlsf_block_size(block));
}

/**
 * @brief 链接当前块和下一个物理块
 * @param block 当前块指针
 * @return 下一个块的指针
 * 
 * 功能：设置下一块的prev_phys_block指针，建立物理连接关系
 * 注意：包含边界检查，避免越界写入
 */
static inline block_header_t* tlsf_block_link_next(block_header_t* block)
{
    block_header_t* next = tlsf_block_next(block);
    /* 仅在 next 在堆内时设置 prev_phys_block，以免写越界 */
    if (tlsf_block_within_heap(next)) {
        next->prev_phys_block = block;
    }
    return next;
}

/**
 * @brief 将块标记为空闲并更新下一块的状态
 * @param block 要标记的块指针
 * 
 * 操作：
 * 1. 设置当前块为空闲状态
 * 2. 设置下一块的"前块空闲"标志
 * 3. 建立物理连接关系
 */
static inline void tlsf_block_mark_as_free(block_header_t* block)
{
    block_header_t* next = tlsf_block_link_next(block);
    tlsf_block_set_prev_free(next);
    tlsf_block_set_free(block);
}

/**
 * @brief 将块标记为已分配并更新下一块的状态
 * @param block 要标记的块指针
 */
static inline void tlsf_block_mark_as_used(block_header_t* block)
{
    block_header_t* next = tlsf_block_next(block);
    tlsf_block_set_prev_used(next);
    tlsf_block_set_used(block);
}

/* 映射函数 */
static void tlsf_mapping_insert(size_t size, int* fli, int* sli)
{
    int fl, sl;
    if (size < TLSF_SMALL_BLOCK_SIZE)
    {
        fl = 0;
        sl = tlsf_cast(int, size) / (TLSF_SMALL_BLOCK_SIZE / TLSF_SL_INDEX_COUNT);
    }
    else
    {
        fl = tlsf_fls(size);
        sl = tlsf_cast(int, size >> (fl - TLSF_SL_INDEX_COUNT_LOG2)) ^ (1 << TLSF_SL_INDEX_COUNT_LOG2);
        fl -= (TLSF_FL_INDEX_SHIFT - 1);
    }
    /* Clamp indices to valid ranges to avoid OOB access */
    if (fl < 0) fl = 0;
    if (fl >= TLSF_FL_INDEX_COUNT) fl = TLSF_FL_INDEX_COUNT - 1;
    if (sl < 0) sl = 0;
    if (sl >= TLSF_SL_INDEX_COUNT) sl = TLSF_SL_INDEX_COUNT - 1;

    *fli = fl;
    *sli = sl;
}

static void tlsf_mapping_search(size_t size, int* fli, int* sli)
{
    if (size >= TLSF_SMALL_BLOCK_SIZE)
    {
        int fls = tlsf_fls(size);
        if (fls <= TLSF_SL_INDEX_COUNT_LOG2) {
            /* avoid negative shift */
        } else {
            const size_t round = (1u << (fls - TLSF_SL_INDEX_COUNT_LOG2)) - 1u;
            size += round;
        }
    }
    tlsf_mapping_insert(size, fli, sli);
}

/* 空闲链表操作 */
static void tlsf_remove_free_block(tlsf_control_t* control, block_header_t* block, int fl, int sl)
{
    /* defensive check: ensure fl/sl in range */
    if (fl < 0 || fl >= TLSF_FL_INDEX_COUNT || sl < 0 || sl >= TLSF_SL_INDEX_COUNT) {
        return;
    }
    block_header_t* prev = block->prev_free;
    block_header_t* next = block->next_free;
    
    if (next)
    {
        next->prev_free = prev;
    }
    if (prev)
    {
        prev->next_free = next;
    }

    if (control->blocks[fl][sl] == block)
    {
        control->blocks[fl][sl] = next;

        if (next == NULL)
        {
            /* clear bit safely */
            control->sl_bitmap[fl] &= ~(1U << sl);

            if (!control->sl_bitmap[fl])
            {
                control->fl_bitmap &= ~(1U << fl);
            }
        }
    }
}

static void tlsf_insert_free_block(tlsf_control_t* control, block_header_t* block, int fl, int sl)
{
    /* defensive check: ensure fl/sl in range */
    if (fl < 0 || fl >= TLSF_FL_INDEX_COUNT || sl < 0 || sl >= TLSF_SL_INDEX_COUNT) {
        return;
    }
    block_header_t* current = control->blocks[fl][sl];
    block->next_free = current;
    block->prev_free = NULL;

    if (current)
    {
        current->prev_free = block;
    }

    control->blocks[fl][sl] = block;
    control->fl_bitmap |= (1U << fl);
    control->sl_bitmap[fl] |= (1U << sl);
}

static block_header_t* tlsf_search_suitable_block(tlsf_control_t* control, int* fli, int* sli)
{
    int fl = *fli;
    int sl = *sli;

    unsigned int sl_map = control->sl_bitmap[fl] & (~0U << sl);
    if (!sl_map)
    {
        const unsigned int fl_map = control->fl_bitmap & (~0U << (fl + 1));
        if (!fl_map)
        {
            return NULL;
        }

        fl = tlsf_ffs(fl_map);
        *fli = fl;
        sl_map = control->sl_bitmap[fl];
    }
    sl = tlsf_ffs(sl_map);
    *sli = sl;

    return control->blocks[fl][sl];
}

/* 块分割和合并 */
static block_header_t* tlsf_block_split(tlsf_control_t* control, block_header_t* block, size_t size)
{
    block_header_t* remaining = tlsf_offset_to_block(block, size);

    const size_t remain_size = tlsf_block_size(block) - size;
    /* 如果 remaining 不在堆内，则不进行分割 */
    if (!tlsf_block_within_heap(remaining)) {
        return NULL;
    }

    remaining->size = remain_size;
    tlsf_block_set_free(remaining);
    tlsf_block_set_prev_used(remaining);
    remaining->prev_phys_block = block;

    tlsf_block_set_size(block, size);
    tlsf_block_mark_as_used(block);

    return remaining;
}

static block_header_t* tlsf_block_absorb(block_header_t* prev, block_header_t* block)
{
    prev->size += tlsf_block_size(block);
    tlsf_block_link_next(prev);
    return prev;
}

static block_header_t* tlsf_block_merge_prev(tlsf_control_t* control, block_header_t* block)
{
    if (tlsf_block_is_prev_free(block))
    {
        block_header_t* prev = block->prev_phys_block;
        int fl, sl;
        tlsf_mapping_insert(tlsf_block_size(prev), &fl, &sl);
        tlsf_remove_free_block(control, prev, fl, sl);
        block = tlsf_block_absorb(prev, block);
    }

    return block;
}

static block_header_t* tlsf_block_merge_next(tlsf_control_t* control, block_header_t* block)
{
    block_header_t* next = tlsf_block_next(block);
    /* 如果 next 不在堆中，不能合并 */
    if (!tlsf_block_within_heap(next)) {
        return block;
    }

    if (tlsf_block_is_free(next))
    {
        int fl, sl;
        tlsf_mapping_insert(tlsf_block_size(next), &fl, &sl);
        tlsf_remove_free_block(control, next, fl, sl);
        block = tlsf_block_absorb(block, next);
    }

    return block;
}

/* 全局变量 */
static uint8_t g_tlsf_heap[configTOTAL_HEAP_SIZE];
static tlsf_control_t g_tlsf_control;
static int g_tlsf_initialized = 0;

/* 检查块指针是否在堆区域内（包含结尾哨兵） */
static inline int tlsf_block_within_heap(const block_header_t* block)
{
    const char* b = (const char*)block;
    const char* start = (const char*)g_tlsf_heap;
    const char* end = (const char*)g_tlsf_heap + configTOTAL_HEAP_SIZE;
    return (b >= start && b < end);
}

/* 初始化内存管理系统 */
void htMemInit(void)
{
    if (g_tlsf_initialized)
    {
        return;
    }

    /* 清零控制结构 */
    memset(&g_tlsf_control, 0, sizeof(tlsf_control_t));

    /* 初始化堆区域 */
    const size_t pool_bytes = configTOTAL_HEAP_SIZE;
    
    /* 对齐堆起始地址 */
    char* mem_ptr = (char*)tlsf_align_up((size_t)g_tlsf_heap, TLSF_ALIGN_SIZE);
    const size_t pool_adjusted = pool_bytes - ((size_t)mem_ptr - (size_t)g_tlsf_heap);
    
    if (pool_adjusted < TLSF_BLOCK_HEADER_SIZE || pool_adjusted < TLSF_POOL_OVERHEAD)
    {
        return;
    }

    /* 创建主区域的空闲块 */
    block_header_t* block = (block_header_t*)mem_ptr;
    const size_t size = pool_adjusted - TLSF_POOL_OVERHEAD;

    block->size = size;
    tlsf_block_set_free(block);
    tlsf_block_set_prev_used(block);
    block->prev_phys_block = NULL;

    /* 创建结束标志块 */
    block_header_t* next = tlsf_block_next(block);
    next->size = 0;
    tlsf_block_set_used(next);
    tlsf_block_set_prev_free(next);
    next->prev_phys_block = block;

    /* 插入空闲块到TLSF结构 */
    int fl, sl;
    tlsf_mapping_insert(size, &fl, &sl);
    tlsf_insert_free_block(&g_tlsf_control, block, fl, sl);

    /* 初始化统计信息 */
    g_tlsf_control.total_size = pool_adjusted;
    g_tlsf_control.free_size = size;
    g_tlsf_control.min_free_size = size;

    g_tlsf_initialized = 1;
}

/* 分配内存 */
void *htPortMalloc(size_t size)
{
    if (!g_tlsf_initialized)
    {
        htMemInit();
    }

    const size_t adjust = tlsf_align_up(size, TLSF_ALIGN_SIZE);
    const size_t block_size = adjust ? 
        tlsf_align_up(adjust + TLSF_BLOCK_HEADER_SIZE, TLSF_ALIGN_SIZE) : 0;

    __disable_irq();

    if (adjust && block_size)
    {
        int fl, sl;
        tlsf_mapping_search(block_size, &fl, &sl);
        
        block_header_t* block = tlsf_search_suitable_block(&g_tlsf_control, &fl, &sl);
        if (block)
        {
            const size_t block_size_real = tlsf_block_size(block);
            tlsf_remove_free_block(&g_tlsf_control, block, fl, sl);

            /* 如果块太大，进行分割 */
            if (block_size_real >= block_size + TLSF_BLOCK_HEADER_SIZE + TLSF_ALIGN_SIZE)
            {
                block_header_t* remaining = tlsf_block_split(&g_tlsf_control, block, block_size);
                tlsf_mapping_insert(tlsf_block_size(remaining), &fl, &sl);
                tlsf_insert_free_block(&g_tlsf_control, remaining, fl, sl);
            }
            else
            {
                tlsf_block_mark_as_used(block);
            }

            /* 更新统计信息 */
            g_tlsf_control.free_size -= tlsf_block_size(block);
            if (g_tlsf_control.free_size < g_tlsf_control.min_free_size)
            {
                g_tlsf_control.min_free_size = g_tlsf_control.free_size;
            }

            __enable_irq();
            return tlsf_block_to_ptr(block);
        }
    }

    __enable_irq();
    return NULL;
}

/* 释放内存 */
void htPortFree(void *ptr)
{
    if (!ptr)
    {
        return;
    }

    __disable_irq();

    block_header_t* block = tlsf_block_from_ptr(ptr);
    
    /* 验证指针有效性 */
    if ((char*)block < (char*)g_tlsf_heap || 
        (char*)block >= (char*)g_tlsf_heap + configTOTAL_HEAP_SIZE)
    {
        __enable_irq();
        return;
    }

    /* 记录原始块大小，用于统计更新 */
    const size_t original_size = tlsf_block_size(block);

    tlsf_block_mark_as_free(block);

    /* 合并相邻的空闲块 */
    block = tlsf_block_merge_prev(&g_tlsf_control, block);
    block = tlsf_block_merge_next(&g_tlsf_control, block);

    /* 插入到空闲链表 */
    int fl, sl;
    tlsf_mapping_insert(tlsf_block_size(block), &fl, &sl);
    tlsf_insert_free_block(&g_tlsf_control, block, fl, sl);

    /* 更新统计信息 - 只添加原始释放的块大小 */
    g_tlsf_control.free_size += original_size;

    __enable_irq();
}

/* 获取当前可用堆大小 */
size_t htGetFreeHeapSize(void)
{
    return g_tlsf_control.free_size;
}

/* 获取历史记录的最小可用堆大小 */
size_t htGetMinimumEverFreeHeapSize(void)
{
    return g_tlsf_control.min_free_size;
}

/* 获取总堆大小 */
size_t htGetTotalHeapSize(void)
{
    return configTOTAL_HEAP_SIZE;
}

