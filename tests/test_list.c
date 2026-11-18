#include "unity.h"

#include "htlist.h"

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

static void test_list_remove_ignores_unlinked_item(void)
{
	htListItem_t item;
	htListItemInit(&item);

	TEST_ASSERT_EQUAL(0, htListRemove(&item));
	TEST_ASSERT_NULL(item.pxContainer);
}

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

static void test_empty_list_sentinel_has_no_owner(void)
{
	htList_t list;
	htListInit(&list);

	TEST_ASSERT_NULL(htListGetItemOwner(&list.xListEnd));
	TEST_ASSERT(list.xListEnd.pxContainer == &list);
	TEST_ASSERT_NULL(htListGetHead(&list));
}

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
	/* 链表是带哨兵的环：末项的下一个是 xListEnd，而不是首项 */
	TEST_ASSERT(htListGetNext(&items[2]) == &list.xListEnd);
	TEST_ASSERT(htListGetNext(&list.xListEnd) == &items[0]);
	TEST_ASSERT(htListGetItemContainer(&items[1]) == &list);

	/* 容器指针可以被改写，不影响链内位置 */
	htListSetItemContainer(&items[1], NULL);
	TEST_ASSERT_NULL(htListGetItemContainer(&items[1]));
	htListSetItemContainer(&items[1], &list);

	/* TEST_ASSERT_EQUAL 会把实参求值两次，带副作用的调用必须先赋给变量 */
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
