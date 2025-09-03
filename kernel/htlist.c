#include "htlist.h"
#include <stddef.h> // 添加此行以获取NULL定义

/**
 * 初始化链表
 * @param pxList 要初始化的链表指针
 */
void htListInit(htList_t *pxList)
{
    /* 链表初始化：设置项目数为0，末尾节点为自身引用 */
    pxList->uxNumberOfItems = 0;
    pxList->pxIndex = (htListItem_t *)&(pxList->xListEnd);
    
    /* 末尾节点初始化：值设为最大，前后指针指向自己 */
    pxList->xListEnd.xItemValue = 0xFFFFFFFF;
    pxList->xListEnd.pxNext = (htListItem_t *)&(pxList->xListEnd);
    pxList->xListEnd.pxPrevious = (htListItem_t *)&(pxList->xListEnd);
}

/**
 * 初始化链表项
 * @param pxItem 要初始化的链表项指针
 */
void htListItemInit(htListItem_t *pxItem)
{
    /* 链表项初始化：所有字段置为初始状态 */
    pxItem->xItemValue = 0;
    pxItem->pxNext = NULL;
    pxItem->pxPrevious = NULL;
    pxItem->pvOwner = NULL;
    pxItem->pxContainer = NULL;  
}

/**
 * 按值插入链表项（升序排列）
 * @param pxList 目标链表
 * @param pxNewListItem 要插入的链表项
 */
void htListInsert(htList_t *pxList, htListItem_t *pxNewListItem)
{
    htListItem_t *pxIterator;
    
    /* 查找插入位置：遍历链表直到找到值大于或等于新项目的节点 */
    for(pxIterator = (htListItem_t *)&(pxList->xListEnd);
        pxIterator->pxNext->xItemValue <= pxNewListItem->xItemValue;
        pxIterator = pxIterator->pxNext)
    {
        /* 空循环，找到插入点 */
    }
    
    /* 执行插入操作 */
    pxNewListItem->pxNext = pxIterator->pxNext;
    pxNewListItem->pxPrevious = pxIterator;
    pxIterator->pxNext->pxPrevious = pxNewListItem;
    pxIterator->pxNext = pxNewListItem;
    
    /* 更新链表项信息 */
    pxNewListItem->pxContainer = (void *)pxList;
    (pxList->uxNumberOfItems)++;
}

/**
 * 将链表项插入到链表末尾
 * @param pxList 目标链表
 * @param pxNewListItem 要插入的链表项
 */
void htListInsertEnd(htList_t *pxList, htListItem_t *pxNewListItem)
{
    htListItem_t *pxIndex = pxList->xListEnd.pxPrevious;
    
    /* 执行插入操作 */
    pxNewListItem->pxNext = (htListItem_t *)&(pxList->xListEnd);
    pxNewListItem->pxPrevious = pxIndex;
    pxIndex->pxNext = pxNewListItem;
    pxList->xListEnd.pxPrevious = pxNewListItem;
    
    /* 更新链表项信息 */
    pxNewListItem->pxContainer = (void *)pxList;  
    (pxList->uxNumberOfItems)++;
}

/**
 * 从链表中移除链表项
 * @param pxItemToRemove 要移除的链表项
 * @return 移除后链表中剩余项目的数量
 */
UBaseType_t htListRemove(htListItem_t *pxItemToRemove) 
{
    htList_t *pxList = (htList_t *)pxItemToRemove->pxContainer; 
    
    /* 调整指针，从链表中移除项目 */
    pxItemToRemove->pxPrevious->pxNext = pxItemToRemove->pxNext;
    pxItemToRemove->pxNext->pxPrevious = pxItemToRemove->pxPrevious;
    
    /* 如果当前索引指向被移除的项目，更新索引 */
    if(pxList->pxIndex == pxItemToRemove)
    {
        pxList->pxIndex = pxItemToRemove->pxPrevious;
    }
    
    /* 标记项目不再属于任何链表 */
    pxItemToRemove->pxContainer = NULL;  
    (pxList->uxNumberOfItems)--;
    
    return pxList->uxNumberOfItems;
}

/**
 * 获取链表中的项目数量
 * @param pxList 目标链表
 * @return 链表项目数量
 */
UBaseType_t htListGetLength(htList_t *pxList)
{
    return pxList->uxNumberOfItems;
}

/**
 * 获取链表头部项目
 * @param pxList 目标链表
 * @return 链表头部项目
 */
htListItem_t *htListGetHead(htList_t *pxList)
{
    /* 返回链表的第一个项目 */
    htListItem_t *pxIndex = (htListItem_t *)&(pxList->xListEnd);
    
    /* 如果链表为空，返回NULL */
    if(pxList->uxNumberOfItems == 0)
    {
        return NULL;
    }
    
    /* 返回链表头部项目 */
    return pxIndex->pxNext;
}

/**
 * 获取链表项目的下一个项目
 * @param pxItem 当前链表项目
 * @return 下一个链表项目
 */
htListItem_t *htListGetNext(htListItem_t *pxItem)
{
    return pxItem->pxNext;
}

/**
 * 设置链表项目的所有者
 * @param pxItem 链表项目
 * @param pvOwner 所有者指针
 */
void htListSetItemOwner(htListItem_t *pxItem, void *pvOwner)
{
    pxItem->pvOwner = pvOwner;
}

/**
 * 获取链表项目的所有者
 * @param pxItem 链表项目
 * @return 所有者指针
 */
void *htListGetItemOwner(htListItem_t *pxItem)
{
    return pxItem->pvOwner;
}

/**
 * 设置链表项目的值
 * @param pxItem 链表项目
 * @param xValue 链表项目值
 */
void htListSetItemValue(htListItem_t *pxItem, TickType_t xValue)
{
    pxItem->xItemValue = xValue;
}

/**
 * 获取链表项目的值
 * @param pxItem 链表项目
 * @return 链表项目值
 */
TickType_t htListGetItemValue(htListItem_t *pxItem)
{
    return pxItem->xItemValue;
}

/**
 * 获取链表的第一个项（与GetHead相同，但命名更明确）
 * @param pxList 链表
 * @return 链表的第一个项，如果链表为空则返回NULL
 */
htListItem_t *htListGetFirstListItem(htList_t *pxList)
{
    return htListGetHead(pxList);
}

/**
 * 获取链表中当前项的数量
 * @param pxList 链表
 * @return 链表中的项数量
 */
UBaseType_t htListGetCurrentNumberOfItems(htList_t *pxList)
{
    return pxList->uxNumberOfItems;
}

/**
 * 设置列表项的容器
 * @param pxListItem 列表项指针
 * @param pxList 容器列表指针
 */
void htListSetItemContainer(htListItem_t *pxListItem, htList_t *pxList)
{
    /* 将列表项的容器指针指向指定列表 */
    pxListItem->pxContainer = (void *)pxList;
}

/**
 * 获取列表项的容器
 * @param pxListItem 列表项指针
 * @return 容器列表指针
 */
htList_t *htListGetItemContainer(htListItem_t *pxListItem)
{
    return (htList_t *)pxListItem->pxContainer;
}
