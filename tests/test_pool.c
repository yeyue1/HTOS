/**
 * @file tests/test_pool.c
 * @brief 确定性 O(1) 定长内存块池（kernel/htpool.c）的主机侧单元测试。
 *
 * 覆盖：8 字节对齐与未对齐起始缓冲处理、全量分配耗尽与逆序/乱序完整回收、
 * 历史最小空闲块水位统计、FromISR 接口以及越界/未对齐/超额释放拦截。
 */
#include "unity.h"

#include <stdint.h>
#include <string.h>

#include "htpool.h"
#include "htscheduler.h"

volatile UBaseType_t uxCriticalNesting = 0U;
static UBaseType_t s_isr_critical_nesting = 0U;

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

UBaseType_t htEnterCriticalFromISR(void)
{
	const UBaseType_t uxPrev = s_isr_critical_nesting;
	s_isr_critical_nesting++;
	return uxPrev;
}

void htExitCriticalFromISR(UBaseType_t uxSavedStatus)
{
	s_isr_critical_nesting = uxSavedStatus;
}

static void test_pool_init_aligns_storage_and_block_size(void)
{
	static uint8_t s_raw[256];
	htPool_t xPool;

	/* 传入故意偏移 +1 字节的未对齐缓冲区，验证自动向上对齐到 8 字节。 */
	TEST_ASSERT_EQUAL(htPASS, htPoolInit(&xPool, s_raw + 1U, sizeof(s_raw) - 1U, 13U));
	TEST_ASSERT_EQUAL(16U, htPoolGetBlockSize(&xPool));
	TEST_ASSERT(htPoolGetTotalCount(&xPool) >= 14U);
	TEST_ASSERT_EQUAL(htPoolGetTotalCount(&xPool), htPoolGetFreeCount(&xPool));
	TEST_ASSERT_EQUAL(htPoolGetTotalCount(&xPool), htPoolGetMinFreeCount(&xPool));

	void *pvFirst = htPoolAlloc(&xPool);
	TEST_ASSERT_NOT_NULL(pvFirst);
	TEST_ASSERT_EQUAL(0U, (size_t)((uintptr_t)pvFirst % HT_POOL_ALIGN_SIZE));
	TEST_ASSERT_EQUAL(htPASS, htPoolFree(&xPool, pvFirst));
	TEST_ASSERT_EQUAL(0U, uxCriticalNesting);
}

static void test_pool_exhaustion_data_integrity_and_min_watermark(void)
{
	static _Alignas(8) uint8_t s_storage[128];
	htPool_t xPool;
	void *apBlocks[8];

	TEST_ASSERT_EQUAL(htPASS, htPoolInit(&xPool, s_storage, sizeof(s_storage), 16U));
	TEST_ASSERT_EQUAL(8U, htPoolGetTotalCount(&xPool));

	for (size_t i = 0U; i < 8U; i++) {
		apBlocks[i] = htPoolAlloc(&xPool);
		TEST_ASSERT_NOT_NULL(apBlocks[i]);
		memset(apBlocks[i], (int)(0xA0U + i), 16U);
	}
	TEST_ASSERT_NULL(htPoolAlloc(&xPool));
	TEST_ASSERT_EQUAL(0U, htPoolGetFreeCount(&xPool));
	TEST_ASSERT_EQUAL(0U, htPoolGetMinFreeCount(&xPool));

	/* 验证各块互不重叠，按交错顺序归还后全部恢复。 */
	for (size_t i = 0U; i < 8U; i++) {
		const uint8_t *pucBytes = (const uint8_t *)apBlocks[i];
		for (size_t k = 0U; k < 16U; k++) {
			TEST_ASSERT_EQUAL((uint8_t)(0xA0U + i), pucBytes[k]);
		}
	}
	for (size_t i = 0U; i < 8U; i++) {
		const size_t idx = (i * 3U) % 8U;
		TEST_ASSERT_EQUAL(htPASS, htPoolFree(&xPool, apBlocks[idx]));
	}
	TEST_ASSERT_EQUAL(8U, htPoolGetFreeCount(&xPool));
	TEST_ASSERT_EQUAL(0U, htPoolGetMinFreeCount(&xPool));
	TEST_ASSERT_EQUAL(0U, uxCriticalNesting);
}

static void test_pool_isr_api_and_invalid_free_rejection(void)
{
	static _Alignas(8) uint8_t s_storage[96];
	uint8_t ucOutside[32];
	htPool_t xPool;

	TEST_ASSERT_EQUAL(htFAIL, htPoolInit(NULL, s_storage, sizeof(s_storage), 16U));
	TEST_ASSERT_EQUAL(htFAIL, htPoolInit(&xPool, s_storage, 8U, 32U));
	TEST_ASSERT_EQUAL(htPASS, htPoolInit(&xPool, s_storage, sizeof(s_storage), 32U));
	TEST_ASSERT_EQUAL(3U, htPoolGetTotalCount(&xPool));

	void *pvBlk = htPoolAllocFromISR(&xPool);
	TEST_ASSERT_NOT_NULL(pvBlk);
	TEST_ASSERT_EQUAL(2U, htPoolGetFreeCount(&xPool));

	/* 拒绝：NULL、池外指针、池内非步长对齐指针。 */
	TEST_ASSERT_EQUAL(htFAIL, htPoolFree(&xPool, NULL));
	TEST_ASSERT_EQUAL(htFAIL, htPoolFree(&xPool, ucOutside));
	TEST_ASSERT_EQUAL(htFAIL, htPoolFree(&xPool, (uint8_t *)pvBlk + 8U));
	TEST_ASSERT_EQUAL(2U, htPoolGetFreeCount(&xPool));

	/* 合法归还后再次多放一次（已满池）也必须被拒绝。 */
	TEST_ASSERT_EQUAL(htPASS, htPoolFreeFromISR(&xPool, pvBlk));
	TEST_ASSERT_EQUAL(3U, htPoolGetFreeCount(&xPool));
	TEST_ASSERT_EQUAL(htFAIL, htPoolFree(&xPool, pvBlk));
	TEST_ASSERT_EQUAL(3U, htPoolGetFreeCount(&xPool));
	TEST_ASSERT_EQUAL(0U, s_isr_critical_nesting);
	TEST_ASSERT_EQUAL(0U, uxCriticalNesting);
}

int main(void)
{
	UnityBegin("test_pool.c");
	RUN_TEST(test_pool_init_aligns_storage_and_block_size);
	RUN_TEST(test_pool_exhaustion_data_integrity_and_min_watermark);
	RUN_TEST(test_pool_isr_api_and_invalid_free_rejection);
	return UnityEnd();
}
