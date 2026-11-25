/**
 * @file htpool.c
 * @brief 确定性 O(1) 定长内存块池实现。
 */
#include <string.h>

#include "htpool.h"
#include "htscheduler.h"

#if (configUSE_BLOCK_POOL == 1)

static size_t prvAlignUp(size_t uxValue, size_t uxAlign)
{
	return (uxValue + (uxAlign - 1U)) & ~(uxAlign - 1U);
}

static void *prvPoolPopLocked(htPool_t *pxPool)
{
	void *pvBlock = pxPool->pvFreeList;
	if (pvBlock == NULL) {
		return NULL;
	}

	void *pvNext = NULL;
	memcpy(&pvNext, pvBlock, sizeof(pvNext));
	pxPool->pvFreeList = pvNext;
	pxPool->uxFreeBlocks--;
	if (pxPool->uxFreeBlocks < pxPool->uxMinFreeBlocks) {
		pxPool->uxMinFreeBlocks = pxPool->uxFreeBlocks;
	}
	return pvBlock;
}

static BaseType_t prvPoolPushLocked(htPool_t *pxPool, void *pvBlock)
{
	const uint8_t *pucBlock = (const uint8_t *)pvBlock;

	if (pxPool->uxBlockSize == 0U || pxPool->uxFreeBlocks >= pxPool->uxTotalBlocks) {
		return htFAIL;
	}
	if (pucBlock < pxPool->pucPoolStart || pucBlock >= pxPool->pucPoolEnd) {
		return htFAIL;
	}
	if (((size_t)(pucBlock - pxPool->pucPoolStart) % pxPool->uxBlockSize) != 0U) {
		return htFAIL;
	}

	void *pvHead = pxPool->pvFreeList;
	memcpy(pvBlock, &pvHead, sizeof(pvHead));
	pxPool->pvFreeList = pvBlock;
	pxPool->uxFreeBlocks++;
	return htPASS;
}

BaseType_t htPoolInit(htPool_t *pxPool, void *pvStorage, size_t uxStorageSize, size_t uxBlockSize)
{
	if (pxPool == NULL || pvStorage == NULL || uxStorageSize == 0U || uxBlockSize == 0U) {
		return htFAIL;
	}

	const size_t uxMinAlign = (sizeof(void *) > HT_POOL_ALIGN_SIZE) ? sizeof(void *) : HT_POOL_ALIGN_SIZE;
	if (uxBlockSize > (SIZE_MAX - (uxMinAlign - 1U))) {
		return htFAIL;
	}
	const size_t uxAlignedBlockSize = prvAlignUp(uxBlockSize, uxMinAlign);

	const uintptr_t uxRawAddr = (uintptr_t)pvStorage;
	const uintptr_t uxAlignedAddr = (uxRawAddr + (uxMinAlign - 1U)) & ~(uintptr_t)(uxMinAlign - 1U);
	const size_t uxAlignLoss = (size_t)(uxAlignedAddr - uxRawAddr);
	if (uxAlignLoss >= uxStorageSize) {
		return htFAIL;
	}

	const size_t uxUsableBytes = uxStorageSize - uxAlignLoss;
	const size_t uxBlockCount = uxUsableBytes / uxAlignedBlockSize;
	if (uxBlockCount == 0U) {
		return htFAIL;
	}

	uint8_t *pucStart = (uint8_t *)uxAlignedAddr;
	memset(pxPool, 0, sizeof(*pxPool));
	pxPool->pucPoolStart = pucStart;
	pxPool->pucPoolEnd = pucStart + (uxBlockCount * uxAlignedBlockSize);
	pxPool->uxBlockSize = uxAlignedBlockSize;
	pxPool->uxTotalBlocks = (UBaseType_t)uxBlockCount;
	pxPool->uxFreeBlocks = (UBaseType_t)uxBlockCount;
	pxPool->uxMinFreeBlocks = (UBaseType_t)uxBlockCount;

	/* 倒序串起侵入式空闲链表，确保首块位于低地址并保持 O(1) 弹出。 */
	void *pvNext = NULL;
	for (size_t uxIdx = uxBlockCount; uxIdx > 0U; uxIdx--) {
		uint8_t *pucBlock = pucStart + ((uxIdx - 1U) * uxAlignedBlockSize);
		memcpy(pucBlock, &pvNext, sizeof(pvNext));
		pvNext = pucBlock;
	}
	pxPool->pvFreeList = pvNext;
	return htPASS;
}

void *htPoolAlloc(htPool_t *pxPool)
{
	if (pxPool == NULL) {
		return NULL;
	}
	htEnterCritical();
	void *pvBlock = prvPoolPopLocked(pxPool);
	htExitCritical();
	return pvBlock;
}

void *htPoolAllocFromISR(htPool_t *pxPool)
{
	if (pxPool == NULL) {
		return NULL;
	}
	const UBaseType_t uxSavedStatus = htEnterCriticalFromISR();
	void *pvBlock = prvPoolPopLocked(pxPool);
	htExitCriticalFromISR(uxSavedStatus);
	return pvBlock;
}

BaseType_t htPoolFree(htPool_t *pxPool, void *pvBlock)
{
	if (pxPool == NULL || pvBlock == NULL) {
		return htFAIL;
	}
	htEnterCritical();
	const BaseType_t xResult = prvPoolPushLocked(pxPool, pvBlock);
	htExitCritical();
	return xResult;
}

BaseType_t htPoolFreeFromISR(htPool_t *pxPool, void *pvBlock)
{
	if (pxPool == NULL || pvBlock == NULL) {
		return htFAIL;
	}
	const UBaseType_t uxSavedStatus = htEnterCriticalFromISR();
	const BaseType_t xResult = prvPoolPushLocked(pxPool, pvBlock);
	htExitCriticalFromISR(uxSavedStatus);
	return xResult;
}

UBaseType_t htPoolGetFreeCount(const htPool_t *pxPool)
{
	if (pxPool == NULL) {
		return 0U;
	}
	htEnterCritical();
	const UBaseType_t uxCount = pxPool->uxFreeBlocks;
	htExitCritical();
	return uxCount;
}

UBaseType_t htPoolGetMinFreeCount(const htPool_t *pxPool)
{
	if (pxPool == NULL) {
		return 0U;
	}
	htEnterCritical();
	const UBaseType_t uxMin = pxPool->uxMinFreeBlocks;
	htExitCritical();
	return uxMin;
}

UBaseType_t htPoolGetTotalCount(const htPool_t *pxPool)
{
	return (pxPool != NULL) ? pxPool->uxTotalBlocks : 0U;
}

size_t htPoolGetBlockSize(const htPool_t *pxPool)
{
	return (pxPool != NULL) ? pxPool->uxBlockSize : 0U;
}

#endif /* configUSE_BLOCK_POOL == 1 */
