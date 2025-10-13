#include "unity.h"

/* 示例被测函数 */
int add(int a, int b)
{
	return a + b;
}

/* 测试用例 */
void test_add_positive_numbers(void)
{
	TEST_ASSERT_EQUAL(5, add(2, 3));
	TEST_ASSERT_EQUAL(10, add(7, 3));
}

void test_add_negative_numbers(void)
{
	TEST_ASSERT_EQUAL(-5, add(-2, -3));
	TEST_ASSERT_EQUAL(0, add(-5, 5));
}

void test_add_zero(void)
{
	TEST_ASSERT_EQUAL(5, add(5, 0));
	TEST_ASSERT_EQUAL(0, add(0, 0));
}

int main(void)
{
	UnityBegin("test_example.c");

	RUN_TEST(test_add_positive_numbers);
	RUN_TEST(test_add_negative_numbers);
	RUN_TEST(test_add_zero);

	return UnityEnd();
}
