/**
 * @file htmem.c
 * @brief TLSF（Two-Level Segregated Fit）内存分配器的实现。
 *
 * 实现原理：
 * 1. 分离适配：空闲块按大小区间分档，挂进不同的双向空闲链表；
 * 2. 双层索引：一级索引 FL 取块大小最高有效位，二级索引 SL 在该区间内再分 16 档，
 *    FL 共 7 档，因此空闲链表总数为 7 × 16 = 112 条，定位一次分配只查位图；
 * 3. 立即合并：释放时立刻与物理相邻的空闲块合并，抑制外部碎片；
 * 4. 位图加速：fl_bitmap / sl_bitmap 配合 tlsf_fls()、tlsf_ffs() 在常数时间内
 *    找到第一个非空链表，无需遍历。
 *
 * 性能特点：
 * - 分配/释放：O(1) 时间复杂度，与堆中块数量无关；
 * - 内存开销：每个块 16 字节块头（32 位目标下为 4 个指针字段）+ 一份控制结构，
 *   另有堆首、堆尾各一个块头；
 * - 碎片控制：立即合并 + 按大小分档，同一档内取链表头，接近最佳适配；
 * - 本实现不支持收缩：释放的内存只回到空闲链表，不归还给系统。
 *
 * @note 所有公共接口都在内核临界区内操作堆结构，可在任务与中断上下文中调用。
 * @note configTOTAL_HEAP_SIZE 为 0 时整套实现被裁掉，公共接口退化为桩函数。
 *
 * @author HTOS Team
 * @date 2025
 */
#include <string.h>
#include "htmem.h"
#include "htconfig.h"
#include "htscheduler.h"

/*
 * 一级索引上限必须能覆盖整个堆：FL 索引由块大小的最高有效位决定，
 * TLSF_FL_INDEX_MAX = 14 表示最大可寻址块为 2^14 = 16 KB，足以容纳默认 12 KB 堆。
 * 若把 configTOTAL_HEAP_SIZE 配得更大，必须同步调大 TLSF_FL_INDEX_MAX，
 * 否则块会被挤压进最高一档，退化成长链表顺序查找，这里用编译期检查挡住这种配置。
 */
#if configTOTAL_HEAP_SIZE > (1U << TLSF_FL_INDEX_MAX)
#error "TLSF_FL_INDEX_MAX does not cover configTOTAL_HEAP_SIZE"
#endif

/*
 * configTOTAL_HEAP_SIZE 为 0 时，整套 TLSF 实现连同 12 KB 的 g_tlsf_heap 一起被
 * 裁掉。公接口仍保留桩函数，因为内核其余部分（htos.c 的初始化、动态创建接口的
 * 失败回滚路径）还在引用它们；缺符号会让“纯静态分配”配置直接链接失败。
 */
#if (configUSE_MALLOC_FAILED_HOOK == 1)
/**
 * @brief 内存分配失败钩子的弱默认实现：什么也不做。
 *
 * @param[in] xWantedSize 本次申请失败的字节数。
 *
 * @note 分配失败本就能通过返回值表达，是否当作致命错误交给应用决定，
 *       所以默认不停机；提供同名强符号即可接管。
 */
__attribute__((weak)) void htMallocFailedHook(size_t xWantedSize)
{
	(void)xWantedSize;
}
#endif

#if (configTOTAL_HEAP_SIZE > 0U)

/**
 * @name TLSF 辅助宏
 * 用于类型转换与数值比较，让块操作代码保持简洁可读。
 * @{
 */

/** @brief 显式类型转换包装，集中表达“此处是有意为之的强制转换”。 */
#define tlsf_cast(t, exp) ((t)(exp))
/** @brief 取两个值中的较小者。 */
#define tlsf_min(a, b) ((a) < (b) ? (a) : (b))
/** @brief 取两个值中的较大者。 */
#define tlsf_max(a, b) ((a) > (b) ? (a) : (b))
/** @} */

/**
 * @brief 判断块头指针是否落在堆区域内（前置声明）。
 * @param[in] block 待检查的块头指针。
 * @return 1 表示在堆内，0 表示在堆外。
 *
 * 后续的块链接、分割、合并都要先做这个检查再解引用，
 * 用于拦住“越过堆尾的哨兵块”“野指针”这类越界访问。
 */
static inline int tlsf_block_within_heap(const block_header_t *block);

/**
 * @name 位操作辅助函数（ARM Cortex-M 兼容版）
 * 不依赖编译器内建函数，纯 C 实现，保证不同工具链下行为一致。
 * 在 TLSF 算法中的两个用途：
 * 1. tlsf_fls()：由块大小算出一级索引 FL；
 * 2. tlsf_ffs()：在位图中找到第一个置位的位，即第一个非空空闲链表。
 * @{
 */

/**
 * @brief 查找最高有效位位置（Find Last Set），由块大小推算 FL 索引。
 *
 * @param[in] word 输入的 32 位无符号整数。
 * @return 最高有效位的位序号（0-31）；输入为 0 时返回 0。
 *
 * 算法原理：二分查找，把候选区间每次折半，检查高半区是否全为 0；
 * 若高半区为 0 则左移屏蔽、把答案压到低半区继续，最多 5 轮。
 * 时间复杂度 O(log 32) = 5 次判断，常数时间，无循环。
 */
static inline int tlsf_fls(unsigned int word)
{
	if (word == 0)
		return 0;

	int bit = 32;
	/* 二分查找：每次排除一半的位 */
	if (!(word & 0xFFFF0000)) {
		word <<= 16;
		bit -= 16;
	} /* 高16位为0，查找低16位 */
	if (!(word & 0xFF000000)) {
		word <<= 8;
		bit -= 8;
	} /* 高8位为0，查找低8位 */
	if (!(word & 0xF0000000)) {
		word <<= 4;
		bit -= 4;
	} /* 高4位为0，查找低4位 */
	if (!(word & 0xC0000000)) {
		word <<= 2;
		bit -= 2;
	} /* 高2位为0，查找低2位 */
	if (!(word & 0x80000000)) {
		bit -= 1;
	} /* 最高位为0，查找次高位 */

	return bit - 1; /* 转换为从0开始的索引 */
}

/**
 * @brief 查找最低有效位位置（Find First Set），用于位图定位第一个非空链表。
 *
 * @param[in] word 输入的 32 位无符号整数（通常是位图）。
 * @return 最低有效位的位序号（0-31）；输入为 0 时返回 -1。
 *
 * 算法原理：与 tlsf_fls() 对称，但改为检查低半区是否全为 0，
 * 是则右移屏蔽、把答案压到高半区继续。同样最多 5 轮判断，常数时间。
 * @note 返回值 -1 是“位图全 0、没有可用空闲链表”的约定信号，
 *       调用方必须判负，否则会被当成合法索引使用。
 */
static inline int tlsf_ffs(unsigned int word)
{
	if (word == 0)
		return -1;

	int bit = 0;
	/* 二分查找：从低位向高位查找 */
	if (!(word & 0x0000FFFF)) {
		word >>= 16;
		bit += 16;
	} /* 低16位为0，查找高16位 */
	if (!(word & 0x000000FF)) {
		word >>= 8;
		bit += 8;
	} /* 低8位为0，查找高8位 */
	if (!(word & 0x0000000F)) {
		word >>= 4;
		bit += 4;
	} /* 低4位为0，查找高4位 */
	if (!(word & 0x00000003)) {
		word >>= 2;
		bit += 2;
	} /* 低2位为0，查找高2位 */
	if (!(word & 0x00000001)) {
		bit += 1;
	} /* 最低位为0，查找次低位 */

	return bit;
}

/**
 * @name 内存对齐辅助函数
 * ARM 处理器访问未对齐数据会降低性能甚至触发异常（视配置而定），
 * 因此块头位置与每个块的尺寸都必须满足 TLSF_ALIGN_SIZE 对齐。
 * @{
 */

/**
 * @brief 把尺寸向上对齐到指定边界。
 *
 * @param[in] size 待对齐的尺寸。
 * @param[in] align 对齐边界，必须是 2 的幂（本内核取 TLSF_ALIGN_SIZE，32 位下为 4）。
 * @return 不小于 size 且是 align 整数倍的最小值。
 *
 * 算法：先加 align - 1 把不足一个边界的部分顶到下一个边界，再用 ~(align - 1)
 * 清除低位。例如对齐到 4 字节：掩码为 ...11111100，5 会变成 8。
 * @warning align 必须是 2 的幂，否则掩码会抹掉不该抹的位。
 */
static inline size_t tlsf_align_up(size_t size, size_t align)
{
	return (size + (align - 1)) & ~(align - 1);
}

/**
 * @name 内存块操作函数集
 * 这些内联函数封装块头的全部位级操作：
 * 1. 块大小的提取与设置（size 字段低两位是状态位，必须保留）；
 * 2. 当前块“空闲/已用”与物理前块“空闲/已用”两类状态的查询与设置；
 * 3. 由物理地址推算出相邻块（块的物理连接关系）；
 * 4. 用户指针与块头指针之间的互相转换。
 *
 * 设计要点：
 * - size 字段同时承载块大小和两个状态位，读写都要做掩码处理；
 * - 全部用位运算，无分支、无函数调用开销；
 * - 都是 static inline，编译后与手写位运算等价，不引入调用栈。
 * @{
 */

/**
 * @brief 读取块的真实大小（字节）。
 *
 * @param[in] block 块头指针。
 * @return 块大小，已屏蔽掉低两位状态位。
 * @note 返回值包含块头自身的字节数，用户可用空间要再减去 TLSF_BLOCK_HEADER_SIZE。
 */
static inline size_t tlsf_block_size(const block_header_t *block)
{
	return block->size & TLSF_SIZE_MASK; /* 屏蔽掉状态位，只保留大小 */
}

/**
 * @brief 改写块大小，同时保留原有的两个状态位。
 *
 * @param[in,out] block 块头指针。
 * @param[in] size 新的块大小（不含状态位；调用方需保证已对齐）。
 * @note 先读出旧 size 取出状态位，再与新大小拼回去，避免误清“当前块空闲”
 *       或“前块空闲”标志——这两个标志一旦丢失，释放时的合并判断就会出错。
 */
static inline void tlsf_block_set_size(block_header_t *block, size_t size)
{
	const size_t oldsize = block->size;
	/* 保留原有状态位，更新大小部分 */
	block->size = size | (oldsize & (TLSF_BLOCK_CURR_FREE | TLSF_BLOCK_PREV_FREE));
}

/**
 * @brief 判断块当前是否空闲。
 *
 * @param[in] block 块头指针。
 * @return 非 0 表示空闲，0 表示已分配（返回的是位掩码值，不要与具体数值比较）。
 */
static inline int tlsf_block_is_free(const block_header_t *block)
{
	return tlsf_cast(int, block->size &TLSF_BLOCK_CURR_FREE);
}

/**
 * @brief 把块标记为空闲（置位 bit0）。
 *
 * @param[in,out] block 块头指针。
 * @note 这里只改状态位，不负责改动空闲链表；真正挂链由 tlsf_insert_free_block() 完成。
 */
static inline void tlsf_block_set_free(block_header_t *block)
{
	block->size |= TLSF_BLOCK_CURR_FREE;
}

/**
 * @brief 把块标记为已分配（清除 bit0）。
 *
 * @param[in,out] block 块头指针。
 * @note 从空闲链表摘除后必须调用本函数，否则该块既在链表上又被分配出去，
 *       下一次分配会把它重复发出去，造成两个用户指针指向同一片内存。
 */
static inline void tlsf_block_set_used(block_header_t *block)
{
	block->size &= ~TLSF_BLOCK_CURR_FREE;
}

/**
 * @brief 判断物理上前一个块是否空闲。
 *
 * @param[in] block 块头指针。
 * @return 非 0 表示前块空闲，0 表示前块已分配或没有前块。
 *
 * 该信息用于释放时的合并优化：置位时可直接向前合并，省去扫描。
 * @note prev_phys_block 为 NULL 表示本块是堆内首块，此时一律返回 0，
 *       防止对 NULL 解引用，也避免把堆外内存当成前块。
 */
static inline int tlsf_block_is_prev_free(const block_header_t *block)
{
	if (block->prev_phys_block) {
		return tlsf_cast(int, block->size &TLSF_BLOCK_PREV_FREE);
	}
	return 0;
}

/**
 * @brief 置位“物理前块空闲”标志。
 *
 * @param[in,out] block 块头指针。
 * @note 只有 prev_phys_block 非空时才置位：堆内首块没有前块，其标志位无意义。
 */
static inline void tlsf_block_set_prev_free(block_header_t *block)
{
	if (block->prev_phys_block) {
		block->size |= TLSF_BLOCK_PREV_FREE;
	}
}

/**
 * @brief 清除“物理前块空闲”标志。
 *
 * @param[in,out] block 块头指针。
 * 用于分配或分割之后：新的后继块其前块已经变成已分配状态。
 */
static inline void tlsf_block_set_prev_used(block_header_t *block)
{
	block->size &= ~TLSF_BLOCK_PREV_FREE;
}

/**
 * @name 指针转换与块导航
 * 处理“用户指针 ↔ 块头指针”的相互转换，以及由块头推算物理相邻块。
 * 块的内存布局为 [块头][用户数据]，因此两者相差固定偏移 TLSF_BLOCK_START_OFFSET。
 * @{
 */

/**
 * @brief 由用户指针反推块头指针。
 *
 * @param[in] ptr 用户数据指针（htPortMalloc() 返回的地址）。
 * @return 对应的块头指针。
 *
 * 原理：用户指针 = 块头指针 + TLSF_BLOCK_START_OFFSET，反向减掉该偏移即可。
 * @warning 只做指针运算，不校验指针是否属于本堆；有效性检查由调用方（htPortFree）
 *          依据堆范围和块头自洽性完成。
 */
static inline block_header_t *tlsf_block_from_ptr(const void *ptr)
{
	return tlsf_cast(block_header_t *, tlsf_cast(char *, ptr) - TLSF_BLOCK_START_OFFSET);
}

/**
 * @brief 由块头指针算出返回给用户的指针。
 *
 * @param[in] block 块头指针。
 * @return 用户数据指针（块头地址 + TLSF_BLOCK_START_OFFSET）。
 */
static inline void *tlsf_block_to_ptr(const block_header_t *block)
{
	return tlsf_cast(void *, tlsf_cast(char *, block) + TLSF_BLOCK_START_OFFSET);
}

/**
 * @brief 在基准指针上加偏移，得到新的块头位置。
 *
 * @param[in] ptr 基准指针（通常是一个块头）。
 * @param[in] size 偏移字节数（通常是当前块大小，即“紧邻的下一块”）。
 * @return 偏移后的块头指针。
 * @note 纯指针运算，不检查结果是否还在堆内，越界判断由调用方做。
 */
static inline block_header_t *tlsf_offset_to_block(const void *ptr, size_t size)
{
	return tlsf_cast(block_header_t *, tlsf_cast(char *, ptr) + size);
}

/**
 * @brief 取得物理上紧邻的下一个块。
 *
 * @param[in] block 当前块头指针。
 * @return 下一块的块头指针。
 *
 * 原理：下一块位置 = 当前块地址 + 当前块大小（块大小包含自身块头，故首尾正好相接）。
 * @note 堆内最后一块的“下一块”是堆尾哨兵块，尺寸为 0；再往后就越界了，
 *       所以调用方常需要配合 tlsf_block_within_heap() 做范围判断。
 */
static inline block_header_t *tlsf_block_next(const block_header_t *block)
{
	return tlsf_offset_to_block(block, tlsf_block_size(block));
}

/**
 * @brief 建立当前块与下一物理块之间的双向连接关系。
 *
 * @param[in,out] block 当前块头指针。
 * @return 下一块的块头指针（无论是否成功写入连接）。
 *
 * 功能：把下一块的 prev_phys_block 指向当前块，让“向后合并”可以反查前驱。
 * @note 含边界检查：只有下一块确实落在堆内时才写它的 prev_phys_block，
 *       否则会写到堆外的任意内存上。堆尾哨兵块也在堆内，因此能被正常链接。
 */
static inline block_header_t *tlsf_block_link_next(block_header_t *block)
{
	/* 获取下一个块 */
	block_header_t *next = tlsf_block_next(block);
	/* 仅在 next 在堆内时设置 prev_phys_block，以免写越界 */
	if (tlsf_block_within_heap(next)) {
		next->prev_phys_block = block;
	}
	return next;
}

/**
 * @brief 把块标记为空闲，并同步更新下一块的状态位。
 *
 * @param[in,out] block 要标记的块头指针。
 *
 * 操作顺序：
 * 1. 与下一块建立物理连接（写下一块的 prev_phys_block）；
 * 2. 给下一块置位“前块空闲”，供其释放时向前合并；
 * 3. 给自己置位“当前块空闲”。
 * @note 这里只改状态位，块本身还没有挂进空闲链表，所以此刻它既不在链表上、
 *       也不能被分配；随后必须调用 tlsf_insert_free_block() 才算真正归还。
 */
static inline void tlsf_block_mark_as_free(block_header_t *block)
{
	/* 链接下一个块 */
	block_header_t *next = tlsf_block_link_next(block);
	tlsf_block_set_prev_free(next);
	tlsf_block_set_free(block);
}

/**
 * @brief 把块标记为已分配，并清除下一块的“前块空闲”标志。
 *
 * @param[in,out] block 要标记的块头指针。
 * 顺序很关键：先改下一块的前块标志，再改自己的状态位，
 * 保证任何时刻都不会出现“下一块以为前面空闲、而实际已分配”的中间态。
 */
static inline void tlsf_block_mark_as_used(block_header_t *block)
{
	block_header_t *next = tlsf_block_next(block);
	tlsf_block_set_prev_used(next);
	tlsf_block_set_used(block);
}

/**
 * @brief 由块大小直接算出它应落在哪个空闲链表（FL, SL）。
 *
 * @param[in] size 块大小（字节，已对齐）。
 * @param[out] fli 一级索引输出。
 * @param[out] sli 二级索引输出。
 *
 * 分档规则：
 * - 小于 TLSF_SMALL_BLOCK_SIZE（256 字节）的小块：全部归入 FL=0，按其大小在
 *   256 字节区间内线性均分成 16 份，即 SL = size / 16；
 * - 不小于 256 字节的大块：FL 取大小的最高有效位（tlsf_fls()），SL 取该位以下的
 *   第 4～7 位，用“右移后异或掉隐含的最高位”提取，这样每 2 倍区间恰好分成 16 档；
 *   最后减去 TLSF_FL_INDEX_SHIFT - 1 把 FL 平移成以 0 为起点的索引起点。
 *
 * @note 末尾做索引钳位：极端尺寸（或调用方传入异常值）时宁可塞进边界档，
 *       也不能让索引越界去访问 blocks[][] 之外的数组元素。
 */
static void tlsf_mapping_insert(size_t size, int *fli, int *sli)
{
	int fl, sl;
	/* 根据块大小计算FL和SL索引 */
	/* size必须至少为TLSF_SMALL_BLOCK_SIZE */
	if (size < TLSF_SMALL_BLOCK_SIZE) {
		/* 对于小块，FL=0，SL按大小线性划分 */
		fl = 0;
		sl = tlsf_cast(int, size) / (TLSF_SMALL_BLOCK_SIZE / TLSF_SL_INDEX_COUNT);
	} else {
		/* 对于大块，使用对数划分FL，SL按大小线性划分 */
		/* 计算FL：找到最高有效位位置 */
		fl = tlsf_fls(size);
		const int shift = fl - TLSF_SL_INDEX_COUNT_LOG2;
		sl = shift >= 0 ? tlsf_cast(int, size >> shift) ^ (1 << TLSF_SL_INDEX_COUNT_LOG2) : 0;
		fl -= (TLSF_FL_INDEX_SHIFT - 1);
	}
	/* Clamp indices to valid ranges to avoid OOB access */
	if (fl < 0)
		fl = 0;
	if (fl >= TLSF_FL_INDEX_COUNT)
		fl = TLSF_FL_INDEX_COUNT - 1;
	if (sl < 0)
		sl = 0;
	if (sl >= TLSF_SL_INDEX_COUNT)
		sl = TLSF_SL_INDEX_COUNT - 1;

	*fli = fl;
	*sli = sl;
}

/**
 * @brief 计算“查找起点”索引：从哪个（FL, SL）开始扫描才能容纳 size。
 *
 * @param[in] size 需要的块大小（含块头，已对齐）。
 * @param[out] fli 一级索引输出。
 * @param[out] sli 二级索引输出。
 *
 * 与 tlsf_mapping_insert() 的区别：本函数先把 size 向上舍入到所在档的边界值，
 * 再交给 tlsf_mapping_insert()。这样算出的档位里所有块的尺寸都不小于 size，
 * 命中该档（或其更高的档）必然满足请求，避免“抽到的块比请求还小”的风险。
 *
 * @note 只有大块需要舍入，且当 fls 不大于二级索引位数（4）时移位会变成负数，
 *       此时直接跳过舍入（小块本身已在 FL=0 内线性分档，不需要额外处理）。
 */
static void tlsf_mapping_search(size_t size, int *fli, int *sli)
{
	if (size >= TLSF_SMALL_BLOCK_SIZE) {
		int fls = tlsf_fls(size);
		if (fls <= TLSF_SL_INDEX_COUNT_LOG2) {
			/* avoid negative shift */
		} else {
			const size_t round = (1u << (fls - TLSF_SL_INDEX_COUNT_LOG2)) - 1u;
			size += round;
		}
	}
	/* 重新映射以获取最终的FL和SL */
	tlsf_mapping_insert(size, fli, sli);
}

/**
 * @name 空闲链表操作
 * 空闲块以双向链表挂在 blocks[FL][SL] 上，两级位图记录哪些链表非空。
 * @{
 */

/**
 * @brief 把一个空闲块从它所在的空闲链表中摘除。
 *
 * @param[in,out] control TLSF 控制结构。
 * @param[in,out] block 要摘除的空闲块（需确认它确实在给定档位的链表上）。
 * @param[in] fl 该块所在的一级索引。
 * @param[in] sl 该块所在的二级索引。
 *
 * 先修好前后节点的双向指针；若被摘的正是链表头，则把链表头指向后继。
 * 链表被摘空时，还要逐级清除 sl_bitmap 与 fl_bitmap —— 位图是 O(1) 搜索的唯一
 * 依据，多一位脏位就会让搜索定位到一条空链表。
 * @note 入口对 fl/sl 做范围检查，越界直接返回：宁可漏摘（内存不会被复用）也
 *       不能拿越界索引去写控制结构之外的数组元素。
 * @note 链表是双向的，无论摘除的是链表头还是中间节点，改几个指针即可，无需搬移数据。
 */
static void tlsf_remove_free_block(tlsf_control_t *control, block_header_t *block, int fl, int sl)
{
	/* defensive check: ensure fl/sl in range */
	if (fl < 0 || fl >= TLSF_FL_INDEX_COUNT || sl < 0 || sl >= TLSF_SL_INDEX_COUNT) {
		return;
	}
	block_header_t *prev = block->prev_free;
	block_header_t *next = block->next_free;
	/* 从链表中移除块 */
	if (next) {
		/* 更新下一个块的前向指针 */
		next->prev_free = prev;
	}
	if (prev) {
		prev->next_free = next;
	}

	if (control->blocks[fl][sl] == block) {
		control->blocks[fl][sl] = next;

		if (next == NULL) {
			/* clear bit safely */
			control->sl_bitmap[fl] &= ~(1U << sl);

			if (!control->sl_bitmap[fl]) {
				control->fl_bitmap &= ~(1U << fl);
			}
		}
	}
}

/**
 * @brief 把一个空闲块插入到指定档位空闲链表的头部。
 *
 * @param[in,out] control TLSF 控制结构。
 * @param[in,out] block 要插入的空闲块（其 size/状态位应已正确设置）。
 * @param[in] fl 一级索引。
 * @param[in] sl 二级索引。
 *
 * 采用头插法：新块成为链表首，原首件的 prev_free 指向它。同时置位
 * fl_bitmap 与 sl_bitmap[fl]，让搜索能立刻发现这条链表非空。
 * @note 入口同样做 fl/sl 范围检查，越界直接返回，避免写坏控制结构。
 * @note 头插顺序与“取链表首”策略配合，保证首次适配位于所属档位内，
 *       整体表现为 O(1) 的近最佳适配。
 */
static void tlsf_insert_free_block(tlsf_control_t *control, block_header_t *block, int fl, int sl)
{
	/* defensive check: ensure fl/sl in range */
	if (fl < 0 || fl >= TLSF_FL_INDEX_COUNT || sl < 0 || sl >= TLSF_SL_INDEX_COUNT) {
		return;
	}

	block_header_t *current = control->blocks[fl][sl];
	block->next_free = current;
	block->prev_free = NULL;

	if (current) {
		current->prev_free = block;
	}

	control->blocks[fl][sl] = block;
	control->fl_bitmap |= (1U << fl);
	control->sl_bitmap[fl] |= (1U << sl);
}

/*
 * 已退役的旧实现：下面保留的是本文件早期版本的 tlsf_search_suitable_block()，
 * 仅作算法思路对照，不参与编译。
 *
 * 它只做“当前 FL 的 SL 位图 → 更高的 FL”一趟扫描，没有任何索引边界检查：
 * 一旦入参 fl/sl 越界（控制结构被写坏，或调用方算错档位），~0U << sl 就是未定义行为，
 * 随后还会越界访问 blocks[][]。现行实现因此加上了范围钳位与 while 重试循环。
 *
 * 旧版算法要点：先在当前 FL 的 SL 位图中找“>= sl”的非空位；找不到就取 fl_bitmap 中
 * 更高 FL 的非空位，并从该 FL 的 SL=0 重新找；最后返回对应链表头。
 * static block_header_t *tlsf_search_suitable_block(tlsf_control_t *control, int *fli, int *sli)
 * {
 * 	int fl = *fli;
 * 	int sl = *sli;
 * 	先在当前 FL 的 SL 位图中查找
 * 	unsigned int sl_map = control->sl_bitmap[fl] & (~0U << sl);
 * 	if (!sl_map) {
 * 		当前 FL 没有合适的 SL，改找更高的 FL
 * 		const unsigned int fl_map = control->fl_bitmap & (~0U << (fl + 1));
 * 		if (!fl_map) {
 * 			return NULL;
 * 		}
 *
 * 		fl = tlsf_ffs(fl_map);
 * 		*fli = fl;
 * 		sl_map = control->sl_bitmap[fl];
 * 	}
 * 	sl = tlsf_ffs(sl_map);
 * 	*sli = sl;
 *
 * 	return control->blocks[fl][sl];
 * }
 */

static block_header_t *tlsf_search_suitable_block(tlsf_control_t *control, int *fli, int *sli)
{
	int fl = *fli;
	int sl = *sli;

	/* 边界检查 */
	if (fl < 0 || fl >= TLSF_FL_INDEX_COUNT)
		return NULL;
	if (sl < 0)
		sl = 0;
	if (sl >= TLSF_SL_INDEX_COUNT)
		sl = TLSF_SL_INDEX_COUNT - 1;

	/* 先在当前 FL 中查找 SL >= sl 的位 */
	unsigned int sl_map = control->sl_bitmap[fl] & (~0U << sl);

	/* 如果当前 FL 没有合适的 SL，循环查找更高的 FL（直到找到或没有更高的 FL） */
	while (!sl_map) {
		const unsigned int fl_map = control->fl_bitmap & (~0U << (fl + 1));
		if (!fl_map) {
			/* 所有更高 FL 都没有空闲链表 */
			return NULL;
		}

		int next_fl = tlsf_ffs(fl_map);
		if (next_fl < 0 || next_fl >= TLSF_FL_INDEX_COUNT) {
			return NULL;
		}

		fl = next_fl;
		/* 在新的 FL 从 SL=0 开始查找 */
		sl_map = control->sl_bitmap[fl];
		if (!sl_map) {
			/* 保险起见继续循环（理论上 fl_map 保证该 FL 有非零 sl_bitmap，但加入循环更稳健） */
			continue;
		}
		break;
	}

	int next_sl = tlsf_ffs(sl_map);
	if (next_sl < 0 || next_sl >= TLSF_SL_INDEX_COUNT) {
		return NULL;
	}

	*fli = fl;
	*sli = next_sl;
	return control->blocks[fl][next_sl];
}

/**
 * @brief 把一个大空闲块切成“前半部分交给分配器、后半部分继续空闲”两块。
 *
 * @param[in,out] block 待分割的空闲块（调用前已从空闲链表摘除）。
 * @param[in] size 前半部分要保留的字节数（含块头，已对齐）。
 * @return 切出来的后半部分块头指针；无法分割时返回 NULL。
 *
 * 后半部分的块头直接落在 block + size 处，把原块剩余的长度、空闲状态、
 * “前块已用”标志和 prev_phys_block 都补齐；前半部分则收缩到 size 并标记为已分配。
 * 这样前后两块在物理上依然首尾相接，后续释放时还能再次合并回来。
 *
 * @note 只有在“剩余长度足够再放一个块头”时调用者才应调用本函数，
 *       这个判断在 htPortMalloc() 里用 block_size_real >= block_size + HEADER + ALIGN 完成。
 * @note 若算出的后半块越出堆范围则放弃分割并返回 NULL：此时宁可整块分配出去，
 *       也不能在堆尾写出一个残缺块头。
 */
static block_header_t *tlsf_block_split(block_header_t *block, size_t size)
{
	/* 剩余块紧接在 block + size 处 */
	block_header_t *remaining = tlsf_offset_to_block(block, size);

	const size_t remain_size = tlsf_block_size(block) - size;
	/* 如果 remaining 不在堆内，则不进行分割 */
	if (!tlsf_block_within_heap(remaining)) {
		return NULL;
	}

	remaining->size = remain_size;
	// 设置剩余块为空闲，前块为已用，初始化时主块前无块
	tlsf_block_set_free(remaining);
	tlsf_block_set_prev_used(remaining);
	remaining->prev_phys_block = block;

	tlsf_block_set_size(block, size);
	tlsf_block_mark_as_used(block);

	return remaining;
}

/**
 * @brief 把物理上紧随 prev 的块 block 并入 prev，只调整块头、不搬移数据。
 *
 * @param[in,out] prev 合并后保留的块（尺寸会变大）。
 * @param[in] block 被并入的块（其块头随合并作废）。
 * @return 合并后的块头指针，即 prev。
 *
 * 合并方式是“把被并块的大小累加给前块，然后重连物理后继”：块头原地不动，
 * 因此已归还的用户区数据不会移动，后继块的前驱指针也能正确更新。
 * @note 调用前被并块必须已从空闲链表摘下，否则链表里会留下指向“已消失块头”的节点。
 */
static block_header_t *tlsf_block_absorb(block_header_t *prev, block_header_t *block)
{
	prev->size += tlsf_block_size(block);
	tlsf_block_link_next(prev);
	return prev;
}

/**
 * @brief 释放时向前合并：若物理前块空闲，则把它并进本块。
 *
 * @param[in,out] control TLSF 控制结构。
 * @param[in,out] block 刚被标记为空闲的块。
 * @return 合并后的块头指针（可能前移到了前块的地址）。
 *
 * 由块头的“前块空闲”标志快速判断，命中后按前块大小算出它的空闲链表档位、
 * 把它摘链，再吸收进本块。这样相邻空闲块在释放瞬间就重新粘成一大块，
 * 避免堆被切成一地小碎片。
 *
 * @note 前块在链表上的档位要重新用 tlsf_mapping_insert() 计算，不能沿用本块的档位。
 * @note 合并后返回的是前块地址，调用方必须使用返回值继续后续操作。
 */
static block_header_t *tlsf_block_merge_prev(tlsf_control_t *control, block_header_t *block)
{
	if (tlsf_block_is_prev_free(block)) {
		block_header_t *prev = block->prev_phys_block;
		int fl, sl;
		/* 按前块大小算出它挂在哪个档位的空闲链表上 */
		tlsf_mapping_insert(tlsf_block_size(prev), &fl, &sl);
		/* 先把它从空闲链表摘下，否则链表里会留下悬空节点 */
		tlsf_remove_free_block(control, prev, fl, sl);
		/* 吸收后块头前移到 prev，返回值随之变化 */
		block = tlsf_block_absorb(prev, block);
	}

	return block;
}

/**
 * @brief 释放时向后合并：若物理后块空闲，则把它并进本块。
 *
 * @param[in,out] control TLSF 控制结构。
 * @param[in,out] block 当前块（此时它已是空闲块或已与前块合并）。
 * @return 合并后的块头指针（本块地址不变）。
 *
 * 先取物理后继并确认它在堆内，再检查其“当前块空闲”标志；命中同样要先按后继
 * 大小算出档位并摘链，然后再吸收。
 * @note 堆尾哨兵块的 size 为 0 且标记为已用，因此天然不会被误并；
 *       越界的“后继”则被 within_heap 检查拦住。
 */
static block_header_t *tlsf_block_merge_next(tlsf_control_t *control, block_header_t *block)
{
	block_header_t *next = tlsf_block_next(block);
	/* 如果 next 不在堆中，不能合并 */
	if (!tlsf_block_within_heap(next)) {
		return block;
	}

	if (tlsf_block_is_free(next)) {
		int fl, sl;
		/* 查找后一个块的位图信息 */
		tlsf_mapping_insert(tlsf_block_size(next), &fl, &sl);
		/* 移除后一个块的空闲链表 */
		tlsf_remove_free_block(control, next, fl, sl);
		block = tlsf_block_absorb(block, next);
	}

	return block;
}

/**
 * @name 全局状态
 * 整个内核共用一份静态堆，不向系统申请内存。
 * @{
 */

/** @brief 堆存储区，按 TLSF_ALIGN_SIZE 对齐；configTOTAL_HEAP_SIZE 为 0 时该数组随实现一起被裁掉。 */
static _Alignas(TLSF_ALIGN_SIZE) uint8_t g_tlsf_heap[configTOTAL_HEAP_SIZE];
/** @brief 空闲链表数组、两级位图与统计信息，全局唯一。 */
static tlsf_control_t g_tlsf_control;
/** @brief 初始化标志，防止 htMemInit() 重复初始化把堆结构清空。 */
static int g_tlsf_initialized = 0;
/** @} */

/**
 * @brief 判断块头指针是否落在堆区间 [g_tlsf_heap, g_tlsf_heap + 大小) 内。
 *
 * @param[in] block 待检查的块头指针（可以是 NULL 或任意野指针）。
 * @return 1 表示在堆内，0 表示在堆外。
 *
 * 用半开区间比较，不访问 block 指向的内容，因此对任意指针都是安全的。
 * @note 判据是“块头起点”在堆内，不保证 block + size 也在堆内，
 *       所以调用方还需要单独检查块尺寸是否越过堆尾（htPortFree 就做了这一步）。
 */
static inline int tlsf_block_within_heap(const block_header_t *block)
{
	const char *b = (const char *)block;
	const char *start = (const char *)g_tlsf_heap;
	const char *end = (const char *)g_tlsf_heap + configTOTAL_HEAP_SIZE;
	return (b >= start && b < end);
}

/**
 * @brief 初始化 TLSF 内存管理器，建立初始空闲块与堆尾哨兵块。
 *
 * @note 幂等：内部用 g_tlsf_initialized 做守卫，重复调用不会重置堆、不会丢失已分配的块。
 * @note 本函数只在系统启动或首次分配时调用；移植层通常由 htOS 初始化流程调用一次。
 *
 * 初始化步骤：
 * 1. 清零控制结构（空闲链表数组、两级位图、统计信息）；
 * 2. 把堆起始地址向上对齐，对齐可能吃掉开头几个字节，可用大小要相应扣减；
 * 3. 在堆首建立一个覆盖几乎整个堆的大空闲块，尾部留出一个块头位置给哨兵；
 * 4. 在堆尾放置哨兵块（size = 0、标记为已用），并用它记录“前块空闲”状态，
 *    使最后一个真实块也能参与向后合并，同时充当越界判断的终点；
 * 5. 把大空闲块挂进空闲链表，初始化 total/free/min_free 统计。
 *
 * @warning 若堆小到连一个块头加一个哨兵都放不下，函数会提前返回且不置初始化标志，
 *          此后所有分配都会失败——这是编译期配置错误，应调整 configTOTAL_HEAP_SIZE。
 */
void htMemInit(void)
{
	if (g_tlsf_initialized) {
		return;
	}

	/* 清零控制结构 */
	memset(&g_tlsf_control, 0, sizeof(tlsf_control_t));

	/* 初始化堆区域 */
	const size_t pool_bytes = configTOTAL_HEAP_SIZE;

	/* 把堆起始地址向上对齐；对齐吃掉的字节数要从可用容量里扣除 */
	char *mem_ptr = (char *)tlsf_align_up((size_t)g_tlsf_heap, TLSF_ALIGN_SIZE);
	const size_t pool_adjusted = pool_bytes - ((size_t)mem_ptr - (size_t)g_tlsf_heap);

	/* 需要至少能放下一个 header 和哨兵 header */
	if (pool_adjusted <= TLSF_BLOCK_HEADER_SIZE * 1) {
		/* 连“一个块 + 一个哨兵块头”都放不下：配置错误，放弃初始化 */
		return;
	}

	/* 计算初始块的总大小（包含当前块头），保留一个尾部 header 作为哨兵 */
	size_t initial_block_size = pool_adjusted - TLSF_BLOCK_HEADER_SIZE; /* 保留尾部 header */

	/* 创建主区域的空闲块（block->size 存储整个块的字节数，包括 header） */
	block_header_t *block = (block_header_t *)mem_ptr;
	block->size = initial_block_size & TLSF_SIZE_MASK;
	/* 当前块空闲、前块已用；初始化时主块之前没有别的块 */
	tlsf_block_set_free(block);
	tlsf_block_set_prev_used(block);
	block->prev_phys_block = NULL;

	/*
	 * 在堆末尾放置哨兵块：size 为 0 且标记为已用，作用是
	 * 1. 充当最后一块的物理后继，使向后合并时不会越过堆尾；
	 * 2. 保存“前块空闲”标志，让最后一个真实块也能被向前合并；
	 * 3. 作为 tlsf_block_within_heap() 判断的右边界参照。
	 */
	block_header_t *next = tlsf_block_next(block);
	/* 确保 next 在堆范围内 */
	if (!tlsf_block_within_heap(next)) {
		/* 不应发生，保护性返回 */
		return;
	}
	next->size = 0;
	/* 当前块空闲、前块已用；初始化时主块之前没有别的块 */
	tlsf_block_set_used(next);
	tlsf_block_set_prev_free(next);
	next->prev_phys_block = block;

	/* 把这块唯一的空闲块按大小挂进空闲链表 */
	int fl, sl;
	tlsf_mapping_insert(tlsf_block_size(block), &fl, &sl);
	tlsf_insert_free_block(&g_tlsf_control, block, fl, sl);

	/* 初始化统计信息 */
	g_tlsf_control.total_size = pool_adjusted;
	g_tlsf_control.free_size = tlsf_block_size(block);
	g_tlsf_control.min_free_size = tlsf_block_size(block);

	g_tlsf_initialized = 1;
}

/**
 * @brief 从静态堆中分配一块内存，全过程在临界区内完成。
 *
 * @param[in] size 请求的字节数。
 * @return 成功返回对齐后的用户指针；堆未初始化、size 为 0 或空间不足时返回 NULL。
 *
 * 处理步骤：
 * 1. 惰性初始化：首次分配时自动调用 htMemInit()；
 * 2. 尺寸校正：把请求长度向上对齐，再加上块头，得到实际需要的块大小 block_size；
 * 3. 档位计算：用 tlsf_mapping_search() 得到保证“块不小于请求”的起始档位；
 * 4. 查找与摘链：查两级位图定位空闲链表并摘下一个块；
 * 5. 分割：剩余部分足够再放一个块头时切出去，重新挂回空闲链表；
 * 6. 统计：从 free_size 减去整块大小，分割后加回剩余部分，并刷新 min_free_size。
 *
 * @note 把 free_size 先减去整块再加回剩余部分，是为了让分割前后的账目都对得上；
 *       若只减实际分配长度，一旦分割失败或整块分配出去，就会算错可用量。
 * @note 临界区覆盖整个搜索 / 摘链 / 分割 / 统计过程，保证多任务与中断并发安全。
 * @warning 返回值只保证 TLSF_ALIGN_SIZE（4 字节）对齐，比 size 更大的对齐需求需自行处理。
 * @warning 分配失败返回 NULL 而不是阻塞等待；需要等待的调用方应自行重试或使用信号量互斥。
 */
void *htPortMalloc(size_t size)
{
	if (!g_tlsf_initialized) {
		htMemInit();
	}

	/* 未初始化、请求长度为 0，或长度大到对齐时会溢出 size_t：一律失败 */
	if (!g_tlsf_initialized || size == 0U || size > SIZE_MAX - (TLSF_ALIGN_SIZE - 1U)) {
		return NULL;
	}

	const size_t adjust = tlsf_align_up(size, TLSF_ALIGN_SIZE);
	if (adjust > SIZE_MAX - TLSF_BLOCK_HEADER_SIZE) {
		return NULL;
	}
	/* 实际块大小 = 对齐后的用户区 + 块头，再整体对齐一次，保证后继块头也满足对齐 */
	const size_t block_size = tlsf_align_up(adjust + TLSF_BLOCK_HEADER_SIZE, TLSF_ALIGN_SIZE);

	htEnterCritical();

	if (block_size <= g_tlsf_control.free_size) {
		int fl, sl;
		tlsf_mapping_search(block_size, &fl, &sl);

		block_header_t *block = tlsf_search_suitable_block(&g_tlsf_control, &fl, &sl);
		if (block) {
			const size_t block_size_real = tlsf_block_size(block);
			tlsf_remove_free_block(&g_tlsf_control, block, fl, sl);

			/* 先从 free_size 减去整个块 */
			g_tlsf_control.free_size -= block_size_real;

			/* 剩余空间还能再放一个“块头 + 最小块”时才值得分割，否则整块给出去 */
			if (block_size_real >= block_size + TLSF_BLOCK_HEADER_SIZE + TLSF_ALIGN_SIZE) {
				block_header_t *remaining = tlsf_block_split(block, block_size);
				/* 剩余块加回 free_size */
				size_t remaining_size = tlsf_block_size(remaining);
				g_tlsf_control.free_size += remaining_size;
				tlsf_mapping_insert(remaining_size, &fl, &sl);
				tlsf_insert_free_block(&g_tlsf_control, remaining, fl, sl);
			} else {
				tlsf_block_mark_as_used(block);
			}

			if (g_tlsf_control.free_size < g_tlsf_control.min_free_size) {
				g_tlsf_control.min_free_size = g_tlsf_control.free_size;
			}

			htExitCritical();
			return tlsf_block_to_ptr(block);
		}
	}

	htExitCritical();

	/* 走到这里说明堆里确实没有足够大的块；上面的尺寸非法属于调用错误，不触发钩子。 */
#if (configUSE_MALLOC_FAILED_HOOK == 1)
	htMallocFailedHook(size);
#endif
	return NULL;
}

/**
 * @brief 释放此前由 htPortMalloc() 返回的内存块，并立即与相邻空闲块合并。
 *
 * @param[in] ptr 待释放的用户指针；传 NULL 时直接返回（允许 free(NULL)）。
 *
 * 释放流程全部在临界区内完成：
 * 1. 由用户指针反推块头；
 * 2. 校验块头落在堆内、尺寸合理（未空闲、不小于块头、按对齐值整数倍、不越过堆尾）；
 * 3. 校验物理前驱一致性（见下）；
 * 4. 标记为空闲，先向前合并、再向后合并；
 * 5. 把合并后的块挂回空闲链表，并只把本次释放的原始大小计入 free_size。
 *
 * @note 第 3 步的“物理前驱一致性”检查是野指针守卫：真实块的 prev_phys_block
 *       要么为 NULL（堆内首块），要么满足 prev + size(prev) == block。堆内的野指针
 *       （例如落在任务栈或数据缓冲区里的随机字节）几乎不可能满足该不变式，
 *       于是在进入空闲链表之前就被拒绝；否则这些字节会被当成块头插进空闲链表，
 *       下一次 malloc 就会把仍在使用的内存分配出去，造成难以定位的踩踏。
 * @note 合并只是把相邻空闲块在物理上重组，总空闲量不变，所以 free_size 只需加上
 *       original_size：邻居本来就已经计在 free_size 里了。
 * @warning 传入非本堆指针、重复释放（块已标记为空闲）、或释放中间指针时，
 *          函数会静默返回而不做任何修改——这是有意的防御行为，但不代表调用正确。
 */
void htPortFree(void *ptr)
{
	if (!ptr) {
		return;
	}

	if (!g_tlsf_initialized || ((uintptr_t)ptr & (TLSF_ALIGN_SIZE - 1U)) != 0U) {
		return;
	}

	htEnterCritical();

	block_header_t *block = tlsf_block_from_ptr(ptr);

	/* 验证指针有效性 */
	if ((char *)block < (char *)g_tlsf_heap || (char *)block >= (char *)g_tlsf_heap + configTOTAL_HEAP_SIZE) {
		htExitCritical();
		return;
	}
	const size_t original_size = tlsf_block_size(block);
	if (tlsf_block_is_free(block) || original_size < TLSF_BLOCK_HEADER_SIZE ||
			(original_size & (TLSF_ALIGN_SIZE - 1U)) != 0U ||
			(char *)block + original_size > (char *)g_tlsf_heap + configTOTAL_HEAP_SIZE) {
		htExitCritical();
		return;
	}

	/*
	 * 物理前驱一致性检查：真实块的 prev_phys_block 要么是 NULL（堆内首块），
	 * 要么满足 prev + size(prev) == block。堆内的野指针（任务栈或数据缓冲区
	 * 里的随机字节）几乎不可能满足该不变式，于是在进入空闲链表之前就被拒绝；
	 * 否则这些字节会被当成块头插进空闲链表，下一次 malloc 就把活跃内存发出去。
	 */
	const block_header_t *prev = block->prev_phys_block;
	if (prev == NULL) {
		if ((char *)block != (char *)tlsf_align_up((size_t)g_tlsf_heap, TLSF_ALIGN_SIZE)) {
			htExitCritical();
			return;
		}
	} else if (!tlsf_block_within_heap(prev) ||
			   (char *)tlsf_offset_to_block(prev, tlsf_block_size(prev)) != (char *)block) {
		htExitCritical();
		return;
	}

	/* 标记当前块为空闲 */
	tlsf_block_mark_as_free(block);

	/* 执行合并（合并只是物理上重组，不改变总空闲量） */
	block = tlsf_block_merge_prev(&g_tlsf_control, block);

	block = tlsf_block_merge_next(&g_tlsf_control, block);

	/* 插入到空闲链表 */
	int fl, sl;
	tlsf_mapping_insert(tlsf_block_size(block), &fl, &sl);
	tlsf_insert_free_block(&g_tlsf_control, block, fl, sl);

	/* 更新统计：只加上本次释放的块大小 */
	/* 关键：无论是否合并，free_size 只需加上新释放的 original_size */
	/* 因为邻居空闲块本来就在 free_size 中，合并不改变总空闲量 */
	g_tlsf_control.free_size += original_size;

	htExitCritical();
}
/**
 * @brief 读取当前空闲堆字节数。
 *
 * @return 当前所有空闲块的总字节数（含块头）。
 * @note 在临界区内取值，保证读到的是某一时刻的完整快照；返回值会随其他任务
 *       的分配释放立刻变化，只适合观察与调试，不能用作分配前的容量判断。
 */
size_t htGetFreeHeapSize(void)
{
	size_t free_size;
	htEnterCritical();
	free_size = g_tlsf_control.free_size;
	htExitCritical();
	return free_size;
}

/**
 * @brief 读取程序启动以来出现过的最小空闲堆字节数。
 *
 * @return 历史最小空闲量（含块头）。
 * @note 该值只减不增，可用于评估内存使用峰值、判断剩余余量是否够用，
 *       也是排查“内存泄漏 / 碎片化严重”的重要指标。
 */
size_t htGetMinimumEverFreeHeapSize(void)
{
	size_t min_free_size;
	htEnterCritical();
	min_free_size = g_tlsf_control.min_free_size;
	htExitCritical();
	return min_free_size;
}

/**
 * @brief 读取堆的总容量。
 *
 * @return 编译期配置的 configTOTAL_HEAP_SIZE，恒为常量。
 * @note 返回的是原始配置值；由于起始地址对齐与堆尾哨兵占用，
 *       实际可用空闲量会略小于该值（见 htGetFreeHeapSize()）。
 */
size_t htGetTotalHeapSize(void)
{
	return configTOTAL_HEAP_SIZE;
}

#else /* configTOTAL_HEAP_SIZE == 0：纯静态分配配置 */

void htMemInit(void) {}

void *htPortMalloc(size_t size)
{
#if (configUSE_MALLOC_FAILED_HOOK == 1)
	/* 纯静态配置下每一次分配都必然失败，正是最需要让钩子发声的场合。 */
	htMallocFailedHook(size);
#else
	(void)size;
#endif
	return NULL;
}

void htPortFree(void *ptr)
{
	(void)ptr;
}

size_t htGetFreeHeapSize(void)
{
	return 0U;
}

size_t htGetMinimumEverFreeHeapSize(void)
{
	return 0U;
}

size_t htGetTotalHeapSize(void)
{
	return 0U;
}

#endif /* configTOTAL_HEAP_SIZE */
