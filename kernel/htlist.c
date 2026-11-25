/**
 * @file htlist.c
 * @brief 双向环形链表实现：按值有序插入、末尾插入、摘除与遍历。
 *
 * 链表用常驻尾哨兵 xListEnd 收口：空链表时 xListEnd.pxNext 与 pxPrevious
 * 都指向自身；非空时 pxNext 指向首元素、pxPrevious 指向尾元素。哨兵的
 * xItemValue 固定为 0xFFFFFFFF（UINT32_MAX），按值升序插入时自然成为上界，
 * 因此空链表不需要单独判断。
 */
#include "htlist.h"
#include <stddef.h> /* 提供 NULL 定义 */

/**
 * @brief 初始化一个空链表。
 *
 * @param[out] pxList 待初始化的链表。
 *
 * @note pxIndex 也指向哨兵，调度器的时间片轮转就是从这里向后取下一个任务。
 */
void htListInit(htList_t *pxList)
{
	htListItem_t *const pxEnd = htListGetEndMarker(pxList);

	/* 链表复位：项目数清零，轮转索引与精简尾哨兵自指构成空环。 */
	pxList->uxNumberOfItems = 0U;
	pxList->pxIndex = pxEnd;
	pxList->xListEnd.xItemValue = UINT32_MAX;
	pxList->xListEnd.pxNext = pxEnd;
	pxList->xListEnd.pxPrevious = pxEnd;
}

/**
 * @brief 初始化一个链表项，使其处于未挂入任何链表的状态。
 *
 * @param[out] pxItem 待初始化的链表项。
 */
void htListItemInit(htListItem_t *pxItem)
{
	/* 逐字段复位：值清零，前后指针、属主与容器全部置空。 */
	pxItem->xItemValue = 0U;
	pxItem->pxNext = NULL;
	pxItem->pxPrevious = NULL;
	pxItem->pvOwner = NULL;
	pxItem->pxContainer = NULL;
}

/**
 * @brief 按 xItemValue 升序把链表项插入链表。
 *
 * @param[in,out] pxList 目标链表。
 * @param[in,out] pxNewListItem 待插入的链表项；插入后其 pxContainer 指向 pxList。
 *
 * @note 键值相同的项插在已有等值项之后，保持插入的先后次序；全程只通过
 *       pxList->xListEnd 直接访问精简哨兵字段，严格遵守 C11 别名规则。
 * @warning 链表项必须已从其它链表摘除，否则会破坏原链表的环。
 */
void htListInsert(htList_t *pxList, htListItem_t *pxNewListItem)
{
	htListItem_t *const pxEnd = htListGetEndMarker(pxList);

	if (pxList->uxNumberOfItems == 0U) {
		pxNewListItem->pxNext = pxEnd;
		pxNewListItem->pxPrevious = pxEnd;
		pxList->xListEnd.pxNext = pxNewListItem;
		pxList->xListEnd.pxPrevious = pxNewListItem;
	} else if (pxNewListItem->xItemValue == UINT32_MAX) {
		/* 最大键值直接插到表尾哨兵之前。 */
		htListItem_t *pxTail = pxList->xListEnd.pxPrevious;
		pxNewListItem->pxNext = pxEnd;
		pxNewListItem->pxPrevious = pxTail;
		pxTail->pxNext = pxNewListItem;
		pxList->xListEnd.pxPrevious = pxNewListItem;
	} else {
		/* 从首元素向后扫描，找到第一个键值严格大于新项的节点（或尾哨兵）。 */
		htListItem_t *pxCursor = pxList->xListEnd.pxNext;
		while (pxCursor != pxEnd && pxCursor->xItemValue <= pxNewListItem->xItemValue) {
			pxCursor = pxCursor->pxNext;
		}

		htListItem_t *pxPrev = (pxCursor == pxEnd) ? pxList->xListEnd.pxPrevious : pxCursor->pxPrevious;
		pxNewListItem->pxNext = pxCursor;
		pxNewListItem->pxPrevious = pxPrev;

		if (pxCursor == pxEnd) {
			pxList->xListEnd.pxPrevious = pxNewListItem;
		} else {
			pxCursor->pxPrevious = pxNewListItem;
		}
		if (pxPrev == pxEnd) {
			pxList->xListEnd.pxNext = pxNewListItem;
		} else {
			pxPrev->pxNext = pxNewListItem;
		}
	}

	pxNewListItem->pxContainer = pxList;
	(pxList->uxNumberOfItems)++;
}

/**
 * @brief 把链表项插入链表末尾（尾哨兵之前）。
 *
 * @param[in,out] pxList 目标链表。
 * @param[in,out] pxNewListItem 待插入的链表项。
 *
 * @note 就绪链表用尾插维持同优先级任务的先来先服务次序。
 */
void htListInsertEnd(htList_t *pxList, htListItem_t *pxNewListItem)
{
	htListItem_t *const pxEnd = htListGetEndMarker(pxList);
	htListItem_t *pxTail = pxList->xListEnd.pxPrevious;

	pxNewListItem->pxNext = pxEnd;
	pxNewListItem->pxPrevious = pxTail;
	if (pxTail == pxEnd) {
		pxList->xListEnd.pxNext = pxNewListItem;
	} else {
		pxTail->pxNext = pxNewListItem;
	}
	pxList->xListEnd.pxPrevious = pxNewListItem;

	pxNewListItem->pxContainer = pxList;
	(pxList->uxNumberOfItems)++;
}

/**
 * @brief 从所属链表中摘除一个链表项。
 *
 * @param[in,out] pxItemToRemove 待摘除的链表项。
 * @return 摘除后链表中的项目数；项目不属于任何链表时返回 0。
 *
 * @note 摘除后 pxContainer/pxNext/pxPrevious 全部清零，重复摘除会直接返回 0。
 */
UBaseType_t htListRemove(htListItem_t *pxItemToRemove)
{
	if (pxItemToRemove == NULL || pxItemToRemove->pxContainer == NULL || pxItemToRemove->pxPrevious == NULL ||
			pxItemToRemove->pxNext == NULL) {
		return 0U;
	}

	htList_t *pxList = pxItemToRemove->pxContainer;
	htListItem_t *const pxEnd = htListGetEndMarker(pxList);

	/* 更新前驱的 pxNext，避免经由 htListItem_t* 写精简哨兵。 */
	if (pxItemToRemove->pxPrevious == pxEnd) {
		pxList->xListEnd.pxNext = pxItemToRemove->pxNext;
	} else {
		pxItemToRemove->pxPrevious->pxNext = pxItemToRemove->pxNext;
	}

	/* 更新后继的 pxPrevious。 */
	if (pxItemToRemove->pxNext == pxEnd) {
		pxList->xListEnd.pxPrevious = pxItemToRemove->pxPrevious;
	} else {
		pxItemToRemove->pxNext->pxPrevious = pxItemToRemove->pxPrevious;
	}

	/* 轮转索引正好指向被摘项目时退回其前驱，避免索引悬空。 */
	if (pxList->pxIndex == pxItemToRemove) {
		pxList->pxIndex = pxItemToRemove->pxPrevious;
	}

	/* 标记项目已不属于任何链表，并同步项目计数。 */
	pxItemToRemove->pxContainer = NULL;
	pxItemToRemove->pxNext = NULL;
	pxItemToRemove->pxPrevious = NULL;
	(pxList->uxNumberOfItems)--;
	return pxList->uxNumberOfItems;
}

/**
 * @brief 返回链表中的项目数。
 *
 * @param[in] pxList 目标链表。
 * @return 链表项目数。
 *
 * @note 直接读取 uxNumberOfItems，开销为 O(1)，不遍历链表。
 */
UBaseType_t htListGetLength(htList_t *pxList)
{
	return pxList->uxNumberOfItems;
}

/**
 * @brief 取链表第一个项目。
 *
 * @param[in] pxList 目标链表。
 * @return 首项目指针；链表为空时返回 NULL。
 */
htListItem_t *htListGetHead(htList_t *pxList)
{
	/* 空链表返回 NULL，避免把哨兵当成真实项目交给调用方。 */
	if (pxList->uxNumberOfItems == 0U) {
		return NULL;
	}

	/* 直接从精简哨兵读取首元素。 */
	return pxList->xListEnd.pxNext;
}

/**
 * @brief 取链表项目的下一个项目。
 *
 * @param[in] pxItem 当前链表项目。
 * @return 下一个项目；当前项目是尾元素时返回尾哨兵而不是 NULL。
 *
 * @note 遍历时需自行与 &pxList->xListEnd 比较来判断结束。
 */
htListItem_t *htListGetNext(htListItem_t *pxItem)
{
	return pxItem->pxNext;
}

/**
 * @brief 设置链表项的属主指针。
 *
 * @param[out] pxItem 目标链表项。
 * @param[in] pvOwner 属主指针：任务链表里即对应 TCB，不参与排序。
 */
void htListSetItemOwner(htListItem_t *pxItem, void *pvOwner)
{
	pxItem->pvOwner = pvOwner;
}

/**
 * @brief 取链表项的属主指针。
 *
 * @param[in] pxItem 目标链表项。
 * @return 属主指针；未设置时为 NULL。
 */
void *htListGetItemOwner(htListItem_t *pxItem)
{
	return pxItem->pvOwner;
}

/**
 * @brief 设置链表项的排序键值。
 *
 * @param[out] pxItem 目标链表项。
 * @param[in] xValue 排序键值，语义由使用方决定。
 *
 * @note 就绪链表存 configMAX_PRIORITIES - 优先级，延时链表存唤醒时刻 tick。
 */
void htListSetItemValue(htListItem_t *pxItem, TickType_t xValue)
{
	pxItem->xItemValue = xValue;
}

/**
 * @brief 取链表项的排序键值。
 *
 * @param[in] pxItem 目标链表项。
 * @return 排序键值。
 */
TickType_t htListGetItemValue(htListItem_t *pxItem)
{
	return pxItem->xItemValue;
}

/**
 * @brief 取链表第一个项目，等价于 htListGetHead()，命名更直白。
 *
 * @param[in] pxList 目标链表。
 * @return 首项目指针；链表为空时返回 NULL。
 */
htListItem_t *htListGetFirstListItem(htList_t *pxList)
{
	return htListGetHead(pxList);
}

/**
 * @brief 返回链表当前项目数，与 htListGetLength() 等价。
 *
 * @param[in] pxList 目标链表。
 * @return 链表中的项目数。
 */
UBaseType_t htListGetCurrentNumberOfItems(htList_t *pxList)
{
	return pxList->uxNumberOfItems;
}

/**
 * @brief 直接设置链表项所属的容器链表。
 *
 * @param[out] pxListItem 目标链表项。
 * @param[in] pxList 新的容器链表。
 *
 * @warning 只改指针、不调整任何链表的环，仅供内核内部修补状态使用。
 */
void htListSetItemContainer(htListItem_t *pxListItem, htList_t *pxList)
{
	/* 把链表项的容器指针指向指定链表。 */
	pxListItem->pxContainer = (void *)pxList;
}

/**
 * @brief 取链表项所属的容器链表。
 *
 * @param[in] pxListItem 目标链表项。
 * @return 容器链表指针；项目未挂入链表时为 NULL。
 */
htList_t *htListGetItemContainer(htListItem_t *pxListItem)
{
	return (htList_t *)pxListItem->pxContainer;
}
