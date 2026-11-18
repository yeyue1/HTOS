/**
 * @file test_example.c
 * @brief Unity 主机测试的最小样板：用 add() 演示断言写法和套件骨架。
 *
 * 本文件不测内核，只是 tests/ 目录的「活样板」：
 * - 一个 TEST 函数只验证一种行为，失败时报错信息能直接指向某类输入；
 * - main() 里的 UnityBegin / RUN_TEST / UnityEnd 三件套就是其余测试文件的最小骨架；
 * - tests/ 全部用例都在主机（MinGW gcc）编译运行，不参与固件链接。
 *
 * @note Unity 的 TEST_ASSERT_EQUAL 会把期望值和实际值各求值一次，
 *       带副作用的实参（如 add(x++, 1)）必须先存到局部变量再断言。
 */
#include "unity.h"

/**
 * @brief 被测样例函数：返回两个整数之和。
 *
 * @param[in] a 第一个加数。
 * @param[in] b 第二个加数。
 * @return 两数之和；溢出行为遵循宿主 C 编译器的有符号整数规则。
 */
int add(int a, int b)
{
	return a + b;
}

/**
 * @brief 两个正数相加，结果必须是精确值。
 *
 * 断言意图：覆盖最基本的正常路径。期望值写在第一个实参位置、实际值写在第二个位置，
 * 写反只会影响失败信息，不影响判定结果。
 */
void test_add_positive_numbers(void)
{
	TEST_ASSERT_EQUAL(5, add(2, 3));
	TEST_ASSERT_EQUAL(10, add(7, 3));
}

/**
 * @brief 两个负数相加，验证符号没有被丢掉。
 *
 * 断言意图：-2 + -3 覆盖「两个操作数同为负」；-5 + 5 覆盖「和为零」这个边界，
 * 防止实现忽略符号位，或把 -0 当成非零处理。
 */
void test_add_negative_numbers(void)
{
	TEST_ASSERT_EQUAL(-5, add(-2, -3));
	TEST_ASSERT_EQUAL(0, add(-5, 5));
}

/**
 * @brief 加 0 不改变操作数（整数加法的单位元）。
 *
 * 断言意图：5 + 0 覆盖单侧为零；0 + 0 覆盖两个操作数都是零的最简退化情形。
 */
void test_add_zero(void)
{
	TEST_ASSERT_EQUAL(5, add(5, 0));
	TEST_ASSERT_EQUAL(0, add(0, 0));
}

/**
 * @brief 套件入口：注册并顺序执行本文件全部用例。
 *
 * @return UnityEnd() 的返回值，即失败的用例数；0 表示全部通过。
 * @note UnityBegin 的实参只是打印用的套件名，与文件名保持一致便于定位失败来源。
 */
int main(void)
{
	UnityBegin("test_example.c");

	RUN_TEST(test_add_positive_numbers);
	RUN_TEST(test_add_negative_numbers);
	RUN_TEST(test_add_zero);

	return UnityEnd();
}
