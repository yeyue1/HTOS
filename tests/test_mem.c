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

int main(void)
{
	UnityBegin("test_mem.c");
	RUN_TEST(test_allocator_reuses_and_coalesces_blocks);
	RUN_TEST(test_allocator_rejects_zero_and_oversized_requests);
	RUN_TEST(test_allocator_ignores_double_free);
	return UnityEnd();
}
