/**
 * @file test_list.c
 * @brief htList 双向循环链表的语义测试：排序插入、摘链、哨兵与导航辅助函数。
 *
 * 覆盖的边界：
 * - 有序插入按 item value 升序（UBaseType_t 是无符号的，最大值不能当哨兵用）；
 * - 摘链必须同时清 pxContainer，对未挂链的项调用必须是安全空操作；
 * - 链表是带哨兵的环：末项的下一个是 xListEnd，哨兵的下一个是首项；
 * - 空链只剩哨兵，而哨兵不属于任何任务。
 *
 * @note 每个用例都在栈上自建 htList_t / htListItem_t，不依赖内核状态，
 *       因此可以在主机上单独运行；断言比较的是项地址，验证的是链接关系本身。
 */
#include "unity.h"

#include "htlist.h"

/**
 * @brief 有序插入按 item value 升序排列，而不是按插入顺序。
 *
 * 断言意图：依次插入值 30、10、20，链上顺序必须变成 10 → 20 → 30。
 * 再用 pxNext 走一遍确认三者的链接：second → third → first，
 * 也就是「head 就是当前最小值」这条不变量成立。
 */
static void test_list_orders_items_by_value(void)
{
	htList_t list;
	htListItem_t first;
	htListItem_t second;
	htListItem_t third;

	htListInit(&list);
	htListItemInit(&first);
	htListItemInit(&second);
	htListItemInit(&third);
	htListSetItemValue(&first, 30U);
	htListSetItemValue(&second, 10U);
	htListSetItemValue(&third, 20U);

	htListInsert(&list, &first);
	htListInsert(&list, &second);
	htListInsert(&list, &third);

	TEST_ASSERT_EQUAL(3, htListGetCurrentNumberOfItems(&list));
	TEST_ASSERT(htListGetHead(&list) == &second);
	TEST_ASSERT(second.pxNext == &third);
	TEST_ASSERT(third.pxNext == &first);
}

/**
 * @brief 摘链必须同时改链接、清容器指针，并返回剩余项数。
 *
 * 断言意图：摘掉 head 后，被摘项的 pxContainer 必须归 NULL——否则该项仍自认为
 * 在链上，后续重复摘链会把邻居的指针写坏；head 必须移到第二项。
 * 摘掉最后一项后 head 归 NULL，两次返回值依次是 1 和 0。
 */
static void test_list_remove_updates_links_and_container(void)
{
	htList_t list;
	htListItem_t first;
	htListItem_t second;

	htListInit(&list);
	htListItemInit(&first);
	htListItemInit(&second);
	htListInsertEnd(&list, &first);
	htListInsertEnd(&list, &second);

	TEST_ASSERT_EQUAL(1, htListRemove(&first));
	TEST_ASSERT_NULL(first.pxContainer);
	TEST_ASSERT(htListGetHead(&list) == &second);
	TEST_ASSERT_EQUAL(0, htListRemove(&second));
	TEST_ASSERT_NULL(htListGetHead(&list));
}

/**
 * @brief 从未挂链的项调用摘链必须是安全空操作。
 *
 * 断言意图：pxContainer 为 NULL 就表示不在任何链上，此时必须返回 0，
 * 并且不得触碰 pxNext/pxPrevious（它们可能指向自身或随机值），
 * 否则一次误调用就会改坏邻接节点的链接。
 */
static void test_list_remove_ignores_unlinked_item(void)
{
	htListItem_t item;
	htListItemInit(&item);

	TEST_ASSERT_EQUAL(0, htListRemove(&item));
	TEST_ASSERT_NULL(item.pxContainer);
}

/**
 * @brief item value 取 UINT32_MAX 时排序仍然正确。
 *
 * 断言意图：延时链表用 value 表示唤醒时刻，UINT32_MAX 就是最远的一个 tick。
 * 攻击点是「拿最大值当尾部哨兵」的实现：若实现把 UINT32_MAX 当链表结尾，
 * 这个真正插入的最大值项就会被静默丢弃，断言会立刻失败。
 */
static void test_list_accepts_maximum_tick_value(void)
{
	htList_t list;
	htListItem_t first;
	htListItem_t maximum;

	htListInit(&list);
	htListItemInit(&first);
	htListItemInit(&maximum);
	htListSetItemValue(&first, 10U);
	htListSetItemValue(&maximum, UINT32_MAX);

	htListInsert(&list, &first);
	htListInsert(&list, &maximum);

	TEST_ASSERT_EQUAL(2, htListGetCurrentNumberOfItems(&list));
	TEST_ASSERT(htListGetHead(&list) == &first);
	TEST_ASSERT(first.pxNext == &maximum);
}

/**
 * @brief 空链表只剩哨兵，而哨兵不属于任何任务。
 *
 * 断言意图：xListEnd 是链表自身的固有成员，不是某次插入的项，因此
 * htListGetItemOwner 必须返回 NULL；同时哨兵的 pxContainer 仍指回所属 list，
 * 说明它由 htListInit 挂好而不是一个游离项。head 必须为 NULL，空链才算空。
 */
static void test_empty_list_sentinel_has_no_owner(void)
{
	htList_t list;
	htListInit(&list);

	TEST_ASSERT_NULL(htListGetItemOwner(&list.xListEnd));
	TEST_ASSERT(list.xListEnd.pxContainer == &list);
	TEST_ASSERT_NULL(htListGetHead(&list));
}

/**
 * @brief 导航辅助函数必须按「带哨兵的环」语义行走。
 *
 * 断言意图：GetFirstListItem 取哨兵的下一个；GetNext 走到末项时返回 xListEnd；
 * 从哨兵再 GetNext 回到首项，说明环是闭合的；GetItemContainer 从项反查所属链。
 * 后段同时验证「改写容器指针不影响链内位置」，以及 TEST_ASSERT_EQUAL 求值两次
 * 的坑——htListRemove 有副作用，必须先存进变量再断言。
 */
static void test_list_navigation_helpers_follow_the_ring(void)
{
	htList_t list;
	htListItem_t items[3];
	UBaseType_t remaining;

	htListInit(&list);
	for (UBaseType_t i = 0U; i < 3U; i++) {
		htListItemInit(&items[i]);
		htListSetItemValue(&items[i], i + 1U);
		htListSetItemOwner(&items[i], &items[i]);
		htListInsert(&list, &items[i]);
	}

	TEST_ASSERT_EQUAL(3, htListGetLength(&list));
	TEST_ASSERT(htListGetFirstListItem(&list) == &items[0]);
	TEST_ASSERT(htListGetNext(&items[0]) == &items[1]);
	/* 链表是带哨兵的环：末项的下一个是 xListEnd，而不是首项，
	   否则遍历没有终止条件；而哨兵的下一个又回到首项，环才是闭合的。 */
	TEST_ASSERT(htListGetNext(&items[2]) == &list.xListEnd);
	TEST_ASSERT(htListGetNext(&list.xListEnd) == &items[0]);
	TEST_ASSERT(htListGetItemContainer(&items[1]) == &list);

	/* 容器指针只是「归属」信息，可以被改写而不影响链内位置：
	   列表本身仍留在 list 的环里，因此随后可以原样还原。 */
	htListSetItemContainer(&items[1], NULL);
	TEST_ASSERT_NULL(htListGetItemContainer(&items[1]));
	htListSetItemContainer(&items[1], &list);

	/* TEST_ASSERT_EQUAL 会把实参求值两次，带副作用的调用必须先赋给变量：
	   htListRemove 会改写链表并返回剩余项数，直接写进断言会被摘两次。 */
	remaining = htListRemove(&items[1]);
	TEST_ASSERT_EQUAL(2, remaining);
	TEST_ASSERT_EQUAL(2, htListGetLength(&list));
	TEST_ASSERT(htListGetNext(&items[0]) == &items[2]);

	remaining = htListRemove(&items[2]);
	TEST_ASSERT_EQUAL(1, remaining);
	remaining = htListRemove(&items[0]);
	TEST_ASSERT_EQUAL(0U, remaining);
	TEST_ASSERT_NULL(htListGetFirstListItem(&list));
}
/**
 * @brief 套件入口：注册并顺序执行本文件全部用例。
 *
 * @return UnityEnd() 的返回值，即失败的用例数；0 表示全部通过。
 */
int main(void)
{
	UnityBegin("test_list.c");
	RUN_TEST(test_list_orders_items_by_value);
	RUN_TEST(test_list_remove_updates_links_and_container);
	RUN_TEST(test_list_remove_ignores_unlinked_item);
	RUN_TEST(test_list_accepts_maximum_tick_value);
	RUN_TEST(test_empty_list_sentinel_has_no_owner);
	RUN_TEST(test_list_navigation_helpers_follow_the_ring);
	return UnityEnd();
}
