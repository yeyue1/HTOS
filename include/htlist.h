/**
 * @file htlist.h
 * @brief 双向循环链表接口。
 *
 * 内核的就绪表、延时表、事件等待表都基于本文件的双向循环链表实现。
 * 链表项（htListItem_t）直接内嵌在任务控制块等对象里，插入/删除不做任何内存分配，
 * 因此这些函数都能在中断上下文里调用。链表带一个固定的尾哨兵 xListEnd，
 * 头尾都指向它，遍历时不会出现 NULL 解引用。
 */
#ifndef HTLIST_H
#define HTLIST_H

#include "httypes.h"

/* ------------------------------------------------------------------ */
/* 1. 列表操作                                                         */
/* ------------------------------------------------------------------ */

/**
 * @brief 初始化一个链表为空表。
 *
 * @param[out] pxList 待初始化的链表，必须指向有效的 htList_t 存储。
 * @note 尾哨兵 xListEnd 的 xItemValue 固定为 0xFFFFFFFF，用于让按值插入自然停在表尾。
 */
void htListInit(htList_t *pxList);

/**
 * @brief 初始化一个链表项为"未挂入任何链表"的状态。
 *
 * @param[out] pxItem 待初始化的链表项。
 * @note 值、所有者、容器全部清零；初始化后不可直接删除，需先插入链表。
 */
void htListItemInit(htListItem_t *pxItem);

/**
 * @brief 把链表项追加到链表末尾，不比较 xItemValue。
 *
 * @param[in,out] pxList 目标链表。
 * @param[in,out] pxNewListItem 待插入的链表项，插入后 pxContainer 指向 pxList。
 * @note 用于就绪表：同优先级任务靠这个顺序实现先来先服务与轮转。
 */
void htListInsertEnd(htList_t *pxList, htListItem_t *pxNewListItem);

/**
 * @brief 把链表项按 xItemValue 升序插入链表，相同值排在已有项之后。
 *
 * @param[in,out] pxList 目标链表。
 * @param[in,out] pxNewListItem 待插入的链表项，插入后 pxContainer 指向 pxList。
 * @note xItemValue 为 0xFFFFFFFF 时直接追加到表尾，避免与尾哨兵比较时死循环；
 *       内核正是利用这一点把"无超时"的任务排在延时表最后。
 */
void htListInsert(htList_t *pxList, htListItem_t *pxNewListItem);

/**
 * @brief 从所在链表中摘除一个链表项。
 *
 * @param[in,out] pxItemToRemove 待摘除的链表项。
 * @return 摘除后链表中剩余的项数；若项指针为 NULL 或本就不在任何链表中，返回 0。
 * @note 摘除后该项的 pxNext/pxPrevious/pxContainer 会被清空，可安全重复调用；
 *       若链表的 pxIndex 正好指向该项，索引会回退到它的前一项。
 */
UBaseType_t htListRemove(htListItem_t *pxItemToRemove);

/* ------------------------------------------------------------------ */
/* 2. 列表项操作                                                       */
/* ------------------------------------------------------------------ */

/**
 * @brief 设置链表项的所有者。
 *
 * @param[out] pxListItem 目标链表项。
 * @param[in] pvOwner 所有者指针，内核里通常传任务的 TCB。
 * @note 内核靠该字段从链表项反查 TCB，因此插入前必须设置。
 */
void htListSetItemOwner(htListItem_t *pxListItem, void *pvOwner);

/**
 * @brief 设置链表项的排序值。
 *
 * @param[out] pxListItem 目标链表项。
 * @param[in] xValue 排序值；用于就绪表时是"优先级取反"，用于延时表时是唤醒 tick。
 * @note 修改已挂入链表的项的值不会重新排序，需先摘除再插入。
 */
void htListSetItemValue(htListItem_t *pxListItem, TickType_t xValue);

/**
 * @brief 读取链表项的排序值。
 *
 * @param[in] pxListItem 目标链表项。
 * @return 该链表项的 xItemValue。
 */
TickType_t htListGetItemValue(htListItem_t *pxListItem);

/**
 * @brief 读取链表项的所有者。
 *
 * @param[in] pxListItem 目标链表项。
 * @return 所有者指针；未设置时为 NULL。
 */
void *htListGetItemOwner(htListItem_t *pxListItem);

/* ------------------------------------------------------------------ */
/* 3. 容器相关操作                                                     */
/* ------------------------------------------------------------------ */

/**
 * @brief 直接把链表项标记为属于某个链表，不做插入动作。
 *
 * @param[out] pxListItem 目标链表项。
 * @param[in] pxList 容器链表。
 * @warning 只改 pxContainer 字段，不会维护链表的项数与前后指针；
 *          正常路径请使用 htListInsert() / htListInsertEnd()。
 */
void htListSetItemContainer(htListItem_t *pxListItem, htList_t *pxList);

/**
 * @brief 读取链表项当前所属的链表。
 *
 * @param[in] pxListItem 目标链表项。
 * @return 容器链表指针；未挂入任何链表时为 NULL。
 */
htList_t *htListGetItemContainer(htListItem_t *pxListItem);

/* ------------------------------------------------------------------ */
/* 4. 列表查询                                                         */
/* ------------------------------------------------------------------ */

/**
 * @brief 取链表头部的链表项。
 *
 * @param[in] pxList 目标链表。
 * @return 第一个链表项；空表返回 NULL（不会返回尾哨兵）。
 */
htListItem_t *htListGetHead(htList_t *pxList);

/**
 * @brief 取链表第一个链表项，语义与 htListGetHead() 相同。
 *
 * @param[in] pxList 目标链表。
 * @return 第一个链表项；空表返回 NULL。
 */
htListItem_t *htListGetFirstListItem(htList_t *pxList);

/**
 * @brief 取链表项的下一个链表项。
 *
 * @param[in] pxItem 当前链表项。
 * @return 下一个链表项；表尾项的下一项是尾哨兵，调用者需自行判断是否已绕回。
 * @warning 不检查 pxItem 是否为 NULL，调用前必须保证指针有效。
 */
htListItem_t *htListGetNext(htListItem_t *pxItem);

/**
 * @brief 取链表中当前的项数。
 *
 * @param[in] pxList 目标链表。
 * @return 链表项数量，空表为 0。
 */
UBaseType_t htListGetCurrentNumberOfItems(htList_t *pxList);

/**
 * @brief 取链表的项数，语义与 htListGetCurrentNumberOfItems() 相同。
 *
 * @param[in] pxList 目标链表。
 * @return 链表项数量，空表为 0。
 */
UBaseType_t htListGetLength(htList_t *pxList);

#endif /* HTLIST_H */
