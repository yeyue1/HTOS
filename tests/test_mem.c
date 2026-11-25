/**
 * @file test_mem.c
 * @brief TLSF 分配器的边界与不变量测试：最大块、耗尽恢复、对齐、非法释放、合并、随机 churn。
 *
 * 覆盖的边界，每条都对应一个曾经或可能出错的分配器不变量：
 * - 上界：最大可分配块，以及「再大就必然失败」的临界点是否精确；
 * - 恢复：整体耗尽、混合尺寸耗尽后归还，空闲量必须精确回到基准值；
 * - 对齐：1..40 字节每种尺寸的返回值都满足 TLSF_ALIGN_SIZE，且各块互不重叠；
 * - 非法输入：0、SIZE_MAX 附近、不小于池子的请求，以及 NULL / 未对齐 / 池外 /
 *   重复释放 / 块内部野指针，全部必须被静默拒绝且不改变堆状态；
 * - 碎片：相邻空闲块必须合并，否则大请求会假性失败；
 * - churn：100 轮随机申请释放后内容不串味、空闲量回到基准、临界区配对。
 *
 * @warning 最后一个用例 test_allocator_rejects_forged_interior_header 是实板 HardFault
 *          的回归测试：旧版 htPortFree 只校验对齐、池内、非空闲与尺寸合理，
 *          无法判断指针是否真由 htPortMalloc 返回，于是池内野指针指向的伪造块头被接受，
 *          伪块进空闲链表并计入 free_size，下一次分配把仍在使用的任务栈发出去，
 *          实板触发 CFSR = 0x00020000（UFSR 的 INVSTATE：跳到 PC = 0 且 T 位为 0），
 *          LR = 0x08000831 指向 htPortFree 里的 htExitCritical 调用点（故障当时在 htmem.c:772）。
 *
 * @note 每个用例结尾都断言 uxCriticalNesting == 0：htPortMalloc/htPortFree 内部成对
 *       调用临界区进出，任何一条提前 return 的路径漏掉退出都会在这里暴露。
 */
#include "unity.h"

#include <stdint.h>

#include "htmem.h"

/**
 * @brief 临界区嵌套计数器，替代 kernel/htscheduler.c 里的同名全局变量。
 *
 * kernel/htmem.c 在 htPortMalloc/htPortFree 里成对调用 htEnterCritical/htExitCritical，
 * 主机测试不链接调度器，因此在这里给出这两个函数的最小实现：只做计数，不真的关中断。
 * 用例末尾断言计数归零，即可证明每一对进出都配上了。
 */
volatile UBaseType_t uxCriticalNesting;

/** @brief 进入临界区：仅递增嵌套计数，主机上没有真实中断可关。 */
void htEnterCritical(void)
{
	uxCriticalNesting++;
}

/** @brief 退出临界区：计数减到 0 为止，防止减穿成负数掩盖配对错误。 */
void htExitCritical(void)
{
	if (uxCriticalNesting > 0U) {
		uxCriticalNesting--;
	}
}

/**
 * @brief 基本分配/释放往返：空闲量随分配下降，全部归还后精确复原。
 *
 * 断言意图：先取基准空闲量，再申请 1024 与 2048 两块，检查返回值非空且都按
 * TLSF_ALIGN_SIZE 对齐，并确认空闲量确实下降；两块按相反顺序归还后，
 * 空闲量必须严格等于基准值——多一字节说明记账出错（块头没回收），
 * 少一字节说明合并出错（空闲块没并回整块）。
 */
static void test_allocator_reuses_and_coalesces_blocks(void)
{
	htMemInit();
	const size_t initial_free = htGetFreeHeapSize();
	void *first = htPortMalloc(1024U);
	void *second = htPortMalloc(2048U);

	TEST_ASSERT_NOT_NULL(first);
	TEST_ASSERT_NOT_NULL(second);
	TEST_ASSERT(((uintptr_t)first % TLSF_ALIGN_SIZE) == 0U);
	TEST_ASSERT(((uintptr_t)second % TLSF_ALIGN_SIZE) == 0U);
	TEST_ASSERT(htGetFreeHeapSize() < initial_free);

	htPortFree(second);
	htPortFree(first);
	TEST_ASSERT_EQUAL(initial_free, htGetFreeHeapSize());
	TEST_ASSERT_EQUAL(0, uxCriticalNesting);
}

/**
 * @brief 0 字节与「不小于整个池子」的请求必须在入池前就被拒绝。
 *
 * 断言意图：malloc(0) 若被放行会返回零长块，后续 split 可能写出块外；
 * 请求 configTOTAL_HEAP_SIZE 必然失败，因为块头本身还要占空间。
 * 两次失败都不能改动空闲量——拒绝路径不允许有任何副作用。
 *
 * @note 本用例故意不调用 htMemInit，沿用上一个用例已经初始化好的堆。
 */
static void test_allocator_rejects_zero_and_oversized_requests(void)
{
	const size_t initial_free = htGetFreeHeapSize();
	TEST_ASSERT_NULL(htPortMalloc(0U));
	TEST_ASSERT_NULL(htPortMalloc(configTOTAL_HEAP_SIZE));
	TEST_ASSERT_EQUAL(initial_free, htGetFreeHeapSize());
	TEST_ASSERT_EQUAL(0, uxCriticalNesting);
}

/**
 * @brief 重复释放同一块不得二次记账。
 *
 * 断言意图：第二次 htPortFree 落在已经是空闲块的地址上，若实现直接把 size
 * 再加进 free_size，空闲量会超过池子容量（实板曾出现 12272 → 24544 的翻倍）。
 * 正确行为是把重复释放当成非法输入忽略，空闲量回到 initial_free。
 */
static void test_allocator_ignores_double_free(void)
{
	const size_t initial_free = htGetFreeHeapSize();
	void *allocation = htPortMalloc(64U);

	TEST_ASSERT_NOT_NULL(allocation);
	htPortFree(allocation);
	htPortFree(allocation);
	TEST_ASSERT_EQUAL(initial_free, htGetFreeHeapSize());
	TEST_ASSERT_EQUAL(0, uxCriticalNesting);
}

/* ------------------------------------------------------------------------- */
/* 边界条件：耗尽、上限、对齐、非法释放、碎片合并、随机折腾               */
/* ------------------------------------------------------------------------- */

/**
 * @brief 二分探测当前堆能给出的最大单次分配，并验证临界点两侧的行为。
 *
 * 探测方式：在 [1, configTOTAL_HEAP_SIZE] 上二分，每次尝试后立即归还探测块，
 * 因此整个探测过程都不改变堆状态——断言 free == base 就是这件事的证据。
 *
 * 断言意图：best 必须大于池子的一半（否则说明 TLSF 映射表存在假性上限，
 * 大块永远分配不出来）；malloc(best) 必须成功，而再大一个对齐单元加一个块头
 * 的请求必须失败，且失败后空闲量回到 base：这一条同时锁定「最大块判定精确」
 * 与「失败路径无副作用」两个性质。
 */
static void test_allocator_reports_the_largest_block(void)
{
	htMemInit();
	const size_t base = htGetFreeHeapSize();
	size_t low = 1U;
	size_t high = configTOTAL_HEAP_SIZE;
	size_t best = 0U;

	while (low <= high) {
		const size_t mid = low + ((high - low) / 2U);
		void *probe = htPortMalloc(mid);

		if (probe != NULL) {
			best = mid;
			htPortFree(probe);
			low = mid + 1U;
		} else {
			high = mid - 1U;
		}
	}

	TEST_ASSERT(best > 0U);
	TEST_ASSERT(best > (configTOTAL_HEAP_SIZE / 2U));
	TEST_ASSERT_EQUAL(base, htGetFreeHeapSize());

	void *whole = htPortMalloc(best);
	TEST_ASSERT_NOT_NULL(whole);
	TEST_ASSERT(htGetFreeHeapSize() < base);
	htPortFree(whole);
	TEST_ASSERT_EQUAL(base, htGetFreeHeapSize());

	/* 比最大块再多一个对齐单元加一个块头，必然失败且不改变堆状态：
	   这条同时验证「临界点精确」与「拒绝路径无副作用」。 */
	TEST_ASSERT_NULL(htPortMalloc(best + TLSF_ALIGN_SIZE + TLSF_BLOCK_HEADER_SIZE));
	TEST_ASSERT_EQUAL(base, htGetFreeHeapSize());
	TEST_ASSERT_EQUAL(0, uxCriticalNesting);
}

/**
 * @brief 用最小块把堆彻底榨干，再逆序归还，空闲量必须精确回到原值。
 *
 * 断言意图：耗尽状态下空闲量必须小于 64 字节，说明真的见底而不是提前失败；
 * 每个块首字节写入自己的序号，逆序归还时逐个回读——合并写坏了活跃数据，
 * 或者把别的块当成自己的来释放，都会在这里串味暴露。
 *
 * @note blocks 声明为 static：512 个指针若放在栈上，主机测试自己就先爆栈了。
 */
static void test_allocator_recovers_from_complete_exhaustion(void)
{
	htMemInit();
	const size_t base = htGetFreeHeapSize();
	static void *blocks[512];
	size_t count = 0U;

	while ((count < 512U) && ((blocks[count] = htPortMalloc(16U)) != NULL)) {
		((uint8_t *)blocks[count])[0] = (uint8_t)(count & 0xFFU);
		count++;
	}

	TEST_ASSERT(count > 100U);
	TEST_ASSERT(htGetFreeHeapSize() < 64U);

	while (count > 0U) {
		count--;
		TEST_ASSERT_EQUAL((uint8_t)(count & 0xFFU), ((uint8_t *)blocks[count])[0]);
		htPortFree(blocks[count]);
	}
	TEST_ASSERT_EQUAL(base, htGetFreeHeapSize());
	TEST_ASSERT_EQUAL(0, uxCriticalNesting);
}

/**
 * @brief 递增尺寸混合申请到失败，再全部归还。
 *
 * 断言意图：与上一个用例互补——尺寸 8、32、56…递增会让每块落在不同的尺寸
 * 分级（FL/SL 双向链表里的不同槽位），验证多个空闲链表同时被清空后仍能完整回收；
 * count >= 4 只是确保确实发生过多次成功分配，避免用例在空堆上空转。
 */
static void test_allocator_recovers_after_mixed_size_exhaustion(void)
{
	htMemInit();
	const size_t base = htGetFreeHeapSize();
	static void *blocks[64];
	size_t count = 0U;

	for (size_t i = 0U; i < 64U; i++) {
		void *block = htPortMalloc(8U + (i * 24U));
		if (block == NULL) {
			break;
		}
		((uint8_t *)block)[0] = (uint8_t)i;
		blocks[count] = block;
		count++;
	}

	TEST_ASSERT(count >= 4U);
	for (size_t i = 0U; i < count; i++) {
		htPortFree(blocks[i]);
	}
	TEST_ASSERT_EQUAL(base, htGetFreeHeapSize());
	TEST_ASSERT_EQUAL(0, uxCriticalNesting);
}

/**
 * @brief 1..40 字节的每种尺寸都必须对齐返回，且各块互不重叠。
 *
 * 断言意图：分配器把请求向上取整到 TLSF_ALIGN_SIZE 的整数倍，奇数尺寸最容易
 * 暴露「块头或负载偏移少算」的错误：一旦两块真的重叠，先写后读的图案校验立刻串味。
 * 图案取 (i * 7 + k)：步长 7 与各块尺寸没有公因数关系，避免周期性巧合掩盖重叠。
 */
static void test_allocator_aligns_every_odd_size(void)
{
	htMemInit();
	const size_t base = htGetFreeHeapSize();
	void *blocks[40];

	for (size_t i = 0U; i < 40U; i++) {
		const size_t size = i + 1U;
		blocks[i] = htPortMalloc(size);
		TEST_ASSERT_NOT_NULL(blocks[i]);
		TEST_ASSERT_EQUAL(0U, (size_t)((uintptr_t)blocks[i] % TLSF_ALIGN_SIZE));
		for (size_t k = 0U; k < size; k++) {
			((uint8_t *)blocks[i])[k] = (uint8_t)((i * 7U) + k);
		}
	}

	for (size_t i = 0U; i < 40U; i++) {
		const size_t size = i + 1U;
		int intact = 1;
		for (size_t k = 0U; k < size; k++) {
			if (((uint8_t *)blocks[i])[k] != (uint8_t)((i * 7U) + k)) {
				intact = 0;
				break;
			}
		}
		TEST_ASSERT(intact);
	}

	for (size_t i = 0U; i < 40U; i++) {
		htPortFree(blocks[i]);
	}
	TEST_ASSERT_EQUAL(base, htGetFreeHeapSize());
	TEST_ASSERT_EQUAL(0, uxCriticalNesting);
}

/**
 * @brief SIZE_MAX 附近的请求必须被拒绝，且不能破坏堆状态。
 *
 * 断言意图：size 加上块头在 size_t 上会回绕成很小的值，若实现「先加后判」，
 * 巨大的请求就变成一次「成功」的小分配——这是经典的整数溢出漏洞。
 * SIZE_MAX-1/-2/-3/-7 分别覆盖回绕后落在不同对齐余数的情形；
 * 最后再试「正好等于池子大小」和「正好等于当前空闲量」，两者都必须失败，
 * 因为块头和负载对齐开销总要让请求比自己能拿到的空间更大。
 */
static void test_allocator_rejects_absurd_sizes(void)
{
	htMemInit();
	const size_t base = htGetFreeHeapSize();

	TEST_ASSERT_NULL(htPortMalloc(SIZE_MAX));
	TEST_ASSERT_NULL(htPortMalloc(SIZE_MAX - 1U));
	TEST_ASSERT_NULL(htPortMalloc(SIZE_MAX - 2U));
	TEST_ASSERT_NULL(htPortMalloc(SIZE_MAX - 3U));
	TEST_ASSERT_NULL(htPortMalloc(SIZE_MAX - 7U));
	TEST_ASSERT_NULL(htPortMalloc((size_t)configTOTAL_HEAP_SIZE));
	TEST_ASSERT_NULL(htPortMalloc(base));
	TEST_ASSERT_EQUAL(base, htGetFreeHeapSize());
	TEST_ASSERT_EQUAL(0, uxCriticalNesting);
}

/**
 * @brief NULL、未对齐、池外、重复释放都必须被静默忽略。
 *
 * 断言意图：local 是栈上数组，属于「堆外指针」，local+1 属于「堆外且未对齐」；
 * 这三种释放若被接受，free_size 会被凭空加上一段，因此断言空闲量一点没变。
 *
 * block+1 是池内但不对齐的指针，必须被拒绝——注意此时块仍处于已分配状态，
 * 所以空闲量仍然小于 base；随后正常释放、再重复释放一次，空闲量才回到 base。
 * 这保证「拒绝非法指针」不会顺手把合法块也放掉。
 */
static void test_allocator_ignores_invalid_free_pointers(void)
{
	htMemInit();
	const size_t base = htGetFreeHeapSize();
	uint8_t local[64];

	htPortFree(NULL);
	htPortFree(local + 1U);
	htPortFree(local);
	TEST_ASSERT_EQUAL(base, htGetFreeHeapSize());

	void *block = htPortMalloc(32U);
	TEST_ASSERT_NOT_NULL(block);
	htPortFree((uint8_t *)block + 1U);
	TEST_ASSERT(htGetFreeHeapSize() < base);
	htPortFree(block);
	htPortFree(block);
	TEST_ASSERT_EQUAL(base, htGetFreeHeapSize());
	TEST_ASSERT_EQUAL(0, uxCriticalNesting);
}

/**
 * @brief 相邻空闲块必须合并，否则耗尽后的小碎片永远凑不出大请求。
 *
 * 断言意图：先用 16 字节小块把堆榨到见底，再释放连续申请的四个块——
 * 它们在物理上相邻，单独任何一个都只有 16 字节，四个合并后才有 64 字节以上，
 * 因此 malloc(100) 成功就等于证明合并真的发生了。
 * 最后归还剩余块，空闲量回到 base，说明合并后的整块也能被完整回收。
 */
static void test_allocator_coalesces_adjacent_blocks(void)
{
	htMemInit();
	const size_t base = htGetFreeHeapSize();
	static void *blocks[512];
	size_t count = 0U;

	while ((count < 512U) && ((blocks[count] = htPortMalloc(16U)) != NULL)) {
		count++;
	}
	TEST_ASSERT(count > 8U);

	/* 连续申请的五个块在物理上相邻：块头复用后每块 16 B 仅占 24 B（32 位）/ 32 B（64 位），
	   单独释放任何一个都只有 16 字节，五个合并后（120 B / 160 B）才够 malloc(100)，
	   因此成功即证明相邻合并生效。 */
	for (size_t i = 0U; i < 5U; i++) {
		htPortFree(blocks[i]);
	}
	void *big = htPortMalloc(100U);
	TEST_ASSERT_NOT_NULL(big);
	htPortFree(big);

	for (size_t i = 5U; i < count; i++) {
		htPortFree(blocks[i]);
	}
	TEST_ASSERT_EQUAL(base, htGetFreeHeapSize());
	TEST_ASSERT_EQUAL(0, uxCriticalNesting);
}

/**
 * @brief 100 轮随机申请/释放：内容不串味，空闲量每轮回到基准。
 *
 * 断言意图：用固定种子的线性同余发生器（rng = rng * 1103515245 + 12345，
 * 种子 0x12345678）产生可复现的伪随机序列，尺寸取 1..300 覆盖多个尺寸分级；
 * 每块写入 seeds[i] + k 的图案，之后按 rng % 16 的随机顺序校验并释放——
 * 随机顺序是关键，它让相邻块以各种先后次序被归还，从而反复触发合并路径。
 *
 * 每轮结束断言空闲量回到 base：任何一次合并漏记、重复记账或尺寸分级错位
 * 都会在这里逐轮累积暴露；最后再确认临界区计数归零。
 */
static void test_allocator_survives_random_churn(void)
{
	htMemInit();
	const size_t base = htGetFreeHeapSize();
	void *slots[16];
	size_t sizes[16];
	uint32_t seeds[16];
	uint32_t rng = 0x12345678U;

	for (uint32_t round = 0U; round < 100U; round++) {
		for (size_t i = 0U; i < 16U; i++) {
			rng = (rng * 1103515245U) + 12345U;
			seeds[i] = rng;
			sizes[i] = 1U + (size_t)(rng % 300U);
			slots[i] = htPortMalloc(sizes[i]);
			TEST_ASSERT_NOT_NULL(slots[i]);
			for (size_t k = 0U; k < sizes[i]; k++) {
				((uint8_t *)slots[i])[k] = (uint8_t)(seeds[i] + k);
			}
		}

		for (size_t n = 0U; n < 16U; n++) {
			rng = (rng * 1103515245U) + 12345U;
			const size_t i = (size_t)(rng % 16U);
			if (slots[i] == NULL) {
				continue;
			}
			int intact = 1;
			for (size_t k = 0U; k < sizes[i]; k++) {
				if (((uint8_t *)slots[i])[k] != (uint8_t)(seeds[i] + k)) {
					intact = 0;
					break;
				}
			}
			TEST_ASSERT(intact);
			htPortFree(slots[i]);
			slots[i] = NULL;
		}

		for (size_t i = 0U; i < 16U; i++) {
			if (slots[i] != NULL) {
				htPortFree(slots[i]);
				slots[i] = NULL;
			}
		}
		TEST_ASSERT_EQUAL(base, htGetFreeHeapSize());
	}
	TEST_ASSERT_EQUAL(0, uxCriticalNesting);
}

/**
 * @brief 块内部的野指针：伪造出「看似合法」的块头后释放，必须被拒绝。
 *
 * 攻击场景：malloc(64) 之后，在用户数据 +16 处写入 prev_phys_block = NULL、
 * size = 0x20，再把 block+32 交给 htPortFree。它正好指向伪造块头的负载起点，
 * 对齐、落在池内、不像已标记使用的块、尺寸也合理——旧实现完全无法分辨，
 * 于是伪块被挂进空闲链表、free_size 被凭空加上 32 字节，下一次 malloc
 * 就把这块仍在使用的内存发了出去，实板直接 HardFault（CFSR = 0x00020000，
 * UFSR 的 INVSTATE，跳到 PC = 0 且 T 位为 0；LR 指向 htPortFree 里的 htExitCritical）。
 *
 * 断言意图：释放必须被拒绝（空闲量保持不变），池子随后仍能正常分配（链表没被写坏），
 * 而真正合法的 block 最后仍要能释放，空闲量回到 base。
 *
 * @note 修复思路（物理前驱一致性守卫）：prev_phys_block == NULL 只允许出现在池内首块，
 *       否则要求 prev 解释出来的块头在池内且 prev + size(prev) == block；
 *       纯软件检查，零 RAM 开销，Flash 只多几十字节。
 */
static void test_allocator_rejects_forged_interior_header(void)
{
	htMemInit();
	const size_t base = htGetFreeHeapSize();
	uint8_t *block = (uint8_t *)htPortMalloc(64U);

	TEST_ASSERT_NOT_NULL(block);
	const size_t after_alloc = htGetFreeHeapSize();

	/* 在 +16 处伪造块头：prev_phys_block = NULL、size = 0x20，
	   于是 block+32 看起来正是这个伪块的负载起点。 */
	uint32_t *forged = (uint32_t *)(block + 16U);
	forged[0] = 0U;
	forged[1] = 0x20U;

	htPortFree(block + 32U); /* 块内部的野指针，必须被拒绝：空闲量不得变化 */
	TEST_ASSERT_EQUAL(after_alloc, htGetFreeHeapSize());

	void *next = htPortMalloc(64U); /* 池子必须仍然可用：伪块没写坏空闲链表 */
	TEST_ASSERT_NOT_NULL(next);
	htPortFree(next);

	htPortFree(block);
	TEST_ASSERT_EQUAL(base, htGetFreeHeapSize());
	TEST_ASSERT_EQUAL(0, uxCriticalNesting);
}

/**
 * @brief 峰值水位只降不升：最小空闲量必须不大于当前空闲量。
 *
 * 断言意图：htGetMinimumEverFreeHeapSize 记录历史最低空闲量，分配之后它必然
 * 小于等于当前空闲量；归还后当前空闲量回到 base，而最小值保持不变
 * （本用例不重置水位，因此只断言两者的大小关系，不断言具体数值）。
 */
static void test_allocator_tracks_peak_usage(void)
{
	htMemInit();
	const size_t base = htGetFreeHeapSize();
	void *block = htPortMalloc(2048U);

	TEST_ASSERT_NOT_NULL(block);
	TEST_ASSERT(htGetFreeHeapSize() < base);
	TEST_ASSERT(htGetMinimumEverFreeHeapSize() <= htGetFreeHeapSize());
	htPortFree(block);
	TEST_ASSERT_EQUAL(base, htGetFreeHeapSize());
	TEST_ASSERT_EQUAL(0, uxCriticalNesting);
}

/**
 * @brief 分裂已释放前块后，其物理后继块仍须可正常释放并回并（验证 prev_phys_block 重连）。
 */
static void test_allocator_relinks_successor_after_split(void)
{
	htMemInit();
	const size_t base = htGetFreeHeapSize();

	/* 8 字节 AAPCS 对齐 + 空闲指针与负载复用（头部开销小于完整 block_header_t）。 */
	TEST_ASSERT_EQUAL(8U, TLSF_ALIGN_SIZE);
	TEST_ASSERT(TLSF_BLOCK_START_OFFSET < sizeof(block_header_t));

	void *first = htPortMalloc(128U);
	void *second = htPortMalloc(64U);
	void *guard = htPortMalloc(32U);
	TEST_ASSERT_NOT_NULL(first);
	TEST_ASSERT_NOT_NULL(second);
	TEST_ASSERT_NOT_NULL(guard);

	/* 释放第一块后，从其内部切出小块触发 tlsf_block_split()，再释放紧邻的 second。 */
	htPortFree(first);
	void *small = htPortMalloc(16U);
	TEST_ASSERT_NOT_NULL(small);

	htPortFree(second);
	htPortFree(small);
	htPortFree(guard);
	TEST_ASSERT_EQUAL(base, htGetFreeHeapSize());
	TEST_ASSERT_EQUAL(0, uxCriticalNesting);
}

/**
 * @brief 套件入口：注册并顺序执行本文件全部用例。
 *
 * @return UnityEnd() 的返回值，即失败的用例数；0 表示全部通过。
 * @note 前三个用例共享同一个已初始化的堆（第二个用例故意不重新 htMemInit），
 *       因此注册顺序不能随意调整；后面每个用例都自己重新初始化并取基准值。
 */
int main(void)
{
	UnityBegin("test_mem.c");
	RUN_TEST(test_allocator_reuses_and_coalesces_blocks);
	RUN_TEST(test_allocator_rejects_zero_and_oversized_requests);
	RUN_TEST(test_allocator_ignores_double_free);

	RUN_TEST(test_allocator_reports_the_largest_block);
	RUN_TEST(test_allocator_recovers_from_complete_exhaustion);
	RUN_TEST(test_allocator_recovers_after_mixed_size_exhaustion);
	RUN_TEST(test_allocator_aligns_every_odd_size);
	RUN_TEST(test_allocator_rejects_absurd_sizes);
	RUN_TEST(test_allocator_ignores_invalid_free_pointers);
	RUN_TEST(test_allocator_coalesces_adjacent_blocks);
	RUN_TEST(test_allocator_survives_random_churn);
	RUN_TEST(test_allocator_rejects_forged_interior_header);
	RUN_TEST(test_allocator_tracks_peak_usage);
	RUN_TEST(test_allocator_relinks_successor_after_split);
	return UnityEnd();
}
