#include "unity.h"

#include <stdint.h>

#include "htmem.h"

volatile UBaseType_t uxCriticalNesting;

void htEnterCritical(void)
{
	uxCriticalNesting++;
}

void htExitCritical(void)
{
	if (uxCriticalNesting > 0U) {
		uxCriticalNesting--;
	}
}

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

static void test_allocator_rejects_zero_and_oversized_requests(void)
{
	const size_t initial_free = htGetFreeHeapSize();
	TEST_ASSERT_NULL(htPortMalloc(0U));
	TEST_ASSERT_NULL(htPortMalloc(configTOTAL_HEAP_SIZE));
	TEST_ASSERT_EQUAL(initial_free, htGetFreeHeapSize());
	TEST_ASSERT_EQUAL(0, uxCriticalNesting);
}

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
/* 边界条件：耗尽、上限、对齐、非法释放、碎片合并、随机腾腾               */
/* ------------------------------------------------------------------------- */

/* 找到当前堆能容纳的最大请求；每次探测后立即归还，堆状态保持不变。 */
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

	/* 比最大块再大一个对齐单元，必然失败且不改变堆状态。 */
	TEST_ASSERT_NULL(htPortMalloc(best + TLSF_ALIGN_SIZE + TLSF_BLOCK_HEADER_SIZE));
	TEST_ASSERT_EQUAL(base, htGetFreeHeapSize());
	TEST_ASSERT_EQUAL(0, uxCriticalNesting);
}

/* 把小内存块分配到完全耗尽，全部归还后空闲量必须精确回到原值。 */
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

/* 混合尺寸耗尽：依次申请递增尺寸到失败，再全部归还。 */
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

/* 任意奇偶尺寸的返回值都必须满足 TLSF_ALIGN_SIZE 对齐，且内存互不重叠。 */
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

/* 荒谬尺寸必须被拒绝，且不能破坏堆状态。 */
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

/* NULL、未对齐、堆外指针、重复释放都必须被静默忽略。 */
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

/* 碎片：只有把相邻空闲块合并起来，大请求才可能成功。 */
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

	/* 连续申请的四个块在物理上相邻，释放后应合并成一个大块。 */
	for (size_t i = 0U; i < 4U; i++) {
		htPortFree(blocks[i]);
	}
	void *big = htPortMalloc(100U);
	TEST_ASSERT_NOT_NULL(big);
	htPortFree(big);

	for (size_t i = 4U; i < count; i++) {
		htPortFree(blocks[i]);
	}
	TEST_ASSERT_EQUAL(base, htGetFreeHeapSize());
	TEST_ASSERT_EQUAL(0, uxCriticalNesting);
}

/* 随机申请/释放 100 轮：内容不得互相污染，空闲量每轮必须回到基准。 */
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

/* 块内部的野指针：用户数据里伪造出“看似合法”的块头，释放时必须被拒绝，
   否则下一次分配会把手上的活跃内存发出去（实板会直接 HardFault）。 */
static void test_allocator_rejects_forged_interior_header(void)
{
	htMemInit();
	const size_t base = htGetFreeHeapSize();
	uint8_t *block = (uint8_t *)htPortMalloc(64U);

	TEST_ASSERT_NOT_NULL(block);
	const size_t after_alloc = htGetFreeHeapSize();

	/* 在 +16 处伪造块头：prev_phys_block = NULL，size = 0x20 */
	uint32_t *forged = (uint32_t *)(block + 16U);
	forged[0] = 0U;
	forged[1] = 0x20U;

	htPortFree(block + 32U); /* 指向块内部的野指针，必须被拒绝 */
	TEST_ASSERT_EQUAL(after_alloc, htGetFreeHeapSize());

	void *next = htPortMalloc(64U); /* 池子必须仍然可用 */
	TEST_ASSERT_NOT_NULL(next);
	htPortFree(next);

	htPortFree(block);
	TEST_ASSERT_EQUAL(base, htGetFreeHeapSize());
	TEST_ASSERT_EQUAL(0, uxCriticalNesting);
}

/* 峰值水位：分配后 min_free 必须不大于当前空闲量，归还后恢复。 */
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
	return UnityEnd();
}
