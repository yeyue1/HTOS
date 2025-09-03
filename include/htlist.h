#ifndef HTLIST_H
#define HTLIST_H

#include "../../include/httypes.h"

/* 列表操作函数原型 */
void htListInit(htList_t *pxList);
void htListItemInit(htListItem_t *pxItem);
void htListInsertEnd(htList_t *pxList, htListItem_t *pxNewListItem);
void htListInsert(htList_t *pxList, htListItem_t *pxNewListItem);
UBaseType_t htListRemove(htListItem_t *pxItemToRemove);

/* 列表项操作函数 */
void htListSetItemOwner(htListItem_t *pxListItem, void *pvOwner);
void htListSetItemValue(htListItem_t *pxListItem, TickType_t xValue);
TickType_t htListGetItemValue(htListItem_t *pxListItem);
void *htListGetItemOwner(htListItem_t *pxListItem);

/* 容器相关函数 */
void htListSetItemContainer(htListItem_t *pxListItem, htList_t *pxList);
htList_t *htListGetItemContainer(htListItem_t *pxListItem);

/* 列表查询函数 */
htListItem_t *htListGetHead(htList_t *pxList);
htListItem_t *htListGetFirstListItem(htList_t *pxList);
UBaseType_t htListGetCurrentNumberOfItems(htList_t *pxList);

#endif /* HTLIST_H */
