/**
 * @file htmem.c
 * @brief 简单内存管理实现
 */
#include <string.h>
#include "htmem.h"
#include "htconfig.h"

/* 内存对齐宏定义 */
#define htBYTES_TO_ALIGN_MASK     (0x03)
#define htBYTES_ALIGNED(x)        ((((size_t)(x)) + htBYTES_TO_ALIGN_MASK) & ~htBYTES_TO_ALIGN_MASK)

/* 块结构体大小 */
#define htBLOCK_SIZE              htBYTES_ALIGNED(sizeof(BlockLink_t))

/* 堆内存定义 */
static uint8_t ucHeap[configTOTAL_HEAP_SIZE];
// 移除不必要的变量，减少内存使用
// static size_t xTotalHeapSize = 0;
static size_t xFreeBytesRemaining = 0;
static size_t xMinimumEverFreeBytesRemaining = 0;

/* 空闲链表开始和结束指针 */
static BlockLink_t xStart, *pxEnd = NULL;
static int heapInitialized = 0; // 添加堆初始化标志

/* 初始化内存管理系统 */
void htMemInit(void)
{
    if(heapInitialized) { // 防止重复初始化
        return;
    }
    
    BlockLink_t *pxFirstFreeBlock;
    uint8_t *pucAlignedHeap;
    size_t uxAddress;
    

    /* 确保堆起始地址对齐 */
    uxAddress = (size_t)ucHeap;
    if ((uxAddress & htBYTES_TO_ALIGN_MASK) != 0)
    {
        uxAddress += (htBYTES_TO_ALIGN_MASK - (uxAddress & htBYTES_TO_ALIGN_MASK));
        pucAlignedHeap = &ucHeap[uxAddress - (size_t)ucHeap];
    }
    else
    {
        pucAlignedHeap = ucHeap;
    }

    /* 初始化空闲链表结构 */
    pxEnd = (BlockLink_t *)((uint8_t *)pucAlignedHeap + configTOTAL_HEAP_SIZE - htBLOCK_SIZE);
    pxEnd->xBlockSize = 0;
    pxEnd->pxNextFreeBlock = NULL;

    /* 初始化起始块 */
    xStart.xBlockSize = 0;
    xStart.pxNextFreeBlock = (BlockLink_t *)pucAlignedHeap;

    /* 初始第一个空闲块 */
    pxFirstFreeBlock = (BlockLink_t *)pucAlignedHeap;
    pxFirstFreeBlock->xBlockSize = configTOTAL_HEAP_SIZE - htBLOCK_SIZE;
    pxFirstFreeBlock->pxNextFreeBlock = pxEnd;

    /* 更新可用内存总量 */
    // xTotalHeapSize = configTOTAL_HEAP_SIZE;  // 不再需要这个变量
    xFreeBytesRemaining = pxFirstFreeBlock->xBlockSize;
    xMinimumEverFreeBytesRemaining = xFreeBytesRemaining;
    
    heapInitialized = 1; // 设置初始化标志

}

/* 分配一个内存块 */
void *htPortMalloc(size_t xWantedSize)
{
    BlockLink_t *pxBlock, *pxPreviousBlock, *pxNewBlockLink;
    void *pvReturn = NULL;
    uint32_t safetyCounter = 0; // 添加安全计数器，防止无限循环
    const uint32_t MAX_LOOP_COUNT = 1000; // 设置循环最大次数限制

    // 简化日志，避免printf循环调用
    if (!heapInitialized)
    {
        htMemInit();
    }
    
    // 先关闭中断
    __disable_irq();
    
    // 使用try-finally结构，确保不管发生什么，中断都会被重新启用
    // 这是C语言中模拟try-finally的方式
    do {
        // 避免请求0字节
        if (xWantedSize == 0) {
            xWantedSize = 1;
        }
        
        // 进行字节对齐
        xWantedSize = htBYTES_ALIGNED(xWantedSize);
        
        // 检查是否有足够的空间
        if (xWantedSize > xFreeBytesRemaining) {
            // 内存不足，退出
            break;
        }
        
        // 从空闲链表开头查找足够大的块
        pxPreviousBlock = &xStart;
        pxBlock = xStart.pxNextFreeBlock;

        // 增加安全检查，防止无限循环
        safetyCounter = 0;
        
        while ((pxBlock->xBlockSize < xWantedSize) && (pxBlock->pxNextFreeBlock != NULL))
        {
            pxPreviousBlock = pxBlock;
            pxBlock = pxBlock->pxNextFreeBlock;
            
            // 安全检查
            if (++safetyCounter > MAX_LOOP_COUNT) {
                // 检测到可能的无限循环
                break;
            }
        }
        
        // 如果安全检查触发，退出
        if (safetyCounter > MAX_LOOP_COUNT) {
            break;
        }

        // 如果我们找到了一个合适的块
        if (pxBlock != pxEnd)
        {
            // 计算返回地址
            pvReturn = (void *)(((uint8_t *)pxBlock) + htBLOCK_SIZE);
            
            // 如果块太大，可以分割
            if ((pxBlock->xBlockSize - xWantedSize) > htBLOCK_SIZE * 2)
            {
                // 创建一个新的空闲块
                pxNewBlockLink = (BlockLink_t *)((uint8_t *)pxBlock + htBLOCK_SIZE + xWantedSize);
                pxNewBlockLink->xBlockSize = pxBlock->xBlockSize - xWantedSize - htBLOCK_SIZE;
                pxNewBlockLink->pxNextFreeBlock = pxBlock->pxNextFreeBlock;
                
                // 更新当前块大小
                pxBlock->xBlockSize = xWantedSize;
                pxBlock->pxNextFreeBlock = pxNewBlockLink;
            }

            // 从空闲链表中移除分配的块
            pxPreviousBlock->pxNextFreeBlock = pxBlock->pxNextFreeBlock;

            // 更新剩余空间
            xFreeBytesRemaining -= pxBlock->xBlockSize;

            // 更新最小剩余空间记录
            if (xFreeBytesRemaining < xMinimumEverFreeBytesRemaining)
            {
                xMinimumEverFreeBytesRemaining = xFreeBytesRemaining;
            }
        }
        
    } while(0); // 只执行一次循环
    
    // 确保中断总是被重新启用（类似finally块）
    __enable_irq();
    

    return pvReturn;
}

/* 释放内存块 - 同样增加安全措施 */
void htPortFree(void *pv)
{
    uint8_t *puc = (uint8_t *)pv;
    BlockLink_t *pxLink;

    if (pv != NULL)
    {
        // 禁用中断
        __disable_irq();
        
        do {
            /* 获取块头 */
            puc -= htBLOCK_SIZE;
            pxLink = (BlockLink_t *)puc;

            /* 检查指针是否有效 - 简单验证 */
            if ((uint8_t*)pxLink < ucHeap || (uint8_t*)pxLink >= ucHeap + configTOTAL_HEAP_SIZE) {
                break;
            }
            
            /* 块大小加回可用空间 */
            xFreeBytesRemaining += pxLink->xBlockSize;

            /* 将块插回空闲链表 - 简单地插入到链表开头 */
            pxLink->pxNextFreeBlock = xStart.pxNextFreeBlock;
            xStart.pxNextFreeBlock = pxLink;
            
        } while(0);
        
        // 恢复中断
        __enable_irq();
    }
    else
    {
    }
}

/* 获取当前可用堆大小 */
size_t htGetFreeHeapSize(void)
{
    return xFreeBytesRemaining;
}

/* 获取历史记录的最小可用堆大小 */
size_t htGetMinimumEverFreeHeapSize(void)
{
    return xMinimumEverFreeBytesRemaining;
}
