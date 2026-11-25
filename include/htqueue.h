/**
 * @file htqueue.h
 * @brief 队列与计数信号量接口。
 *
 * 本文件定义队列控制块 htQUEUE_t 以及队列的一组操作。队列既能搬运数据
 * （uxItemSize > 0，按字节复制），也能退化成计数信号量（uxItemSize == 0，只累加计数）。
 * 生产者/消费者之间的同步通过在等待链表 xTasksWaitingToSend / xTasksWaitingToReceive
 * 上挂起任务实现，超时后由延时表唤醒。
 */
#ifndef HT_QUEUE_H
#define HT_QUEUE_H

#include "httypes.h"
#include "htlist.h"
#include "httask.h"

/**
 * @brief 队列读写指针，只在数据队列（uxItemSize > 0）时有效。
 */
typedef struct htQueuePointers {
	int8_t *pcTail; /**< 指向队列存储区尾部之后的位置 */
	int8_t *pcReadFrom; /**< 指向下一个待读取位置 */
} htQueuePointers_t;

#if (configUSE_RECURSIVE_MUTEXES == 1)
/**
 * @brief 递归互斥量的持有者信息，直接内嵌于控制块 union 中。
 */
typedef struct htMutexHolder {
	htTCB_t *xTaskHandle; /**< 当前持有该互斥量的任务 */
	UBaseType_t uxRecursiveCallCount; /**< 递归获取次数，归零才真正释放 */
	UBaseType_t uxOriginalPriority; /**< 未被继承时的原始优先级 */
} htMutexHolder_t;
#endif

/**
 * @brief 队列控制块。
 *
 * 数据队列与信号量共用同一套结构，靠 uxItemSize 区分：
 * - uxItemSize > 0：使用 u.xQueue 的读写指针搬运数据；
 * - uxItemSize == 0：直接用 uxMessagesWaiting 记录信号量计数，递归互斥量使用 u.xMutex。
 */
typedef struct htQueueDefinition {
	int8_t *pcHead; /**< 指向队列存储区起始位置 */
	int8_t *pcWriteTo; /**< 指向下一个待写入位置 */

	union {
		htQueuePointers_t xQueue; /**< 队列特有的读写指针 */
#if (configUSE_RECURSIVE_MUTEXES == 1)
		htMutexHolder_t xMutex; /**< 递归互斥量内嵌的持有者与继承状态 */
#endif
	} u;

	htList_t xTasksWaitingToSend; /**< 因队列满而等待发送的任务链表 */
	htList_t xTasksWaitingToReceive; /**< 因队列空而等待接收的任务链表 */

	volatile UBaseType_t uxMessagesWaiting; /**< 当前队列中已有消息数（信号量为计数值） */
	UBaseType_t uxLength; /**< 队列容量，即可容纳的项目数 */
	UBaseType_t uxItemSize; /**< 每项字节数，为 0 表示这是信号量或互斥量 */

	uint8_t ucStaticallyAllocated; /**< 非 0 表示控制块与存储区由调用方提供，删除时不释放 */
	uint8_t ucQueueType; /**< 队列子类型（htQUEUE_TYPE_*） */

#if (configUSE_QUEUE_SETS == 1)
	/**
	 * 队列集相关字段，仅在 configUSE_QUEUE_SETS 为 1 时存在。
	 * - pxQueueSetContainer 非空表示本队列是某个集合的成员，每次成功入队都会把自身句柄
	 *   转发进集合；
	 * - uxQueueSetMembers / ucIsQueueSet 只在集合本身（htQueueSetCreate() 创建的队列）
	 *   上有意义，用于拒绝"删掉还有成员的集合"和"把集合再塞进另一个集合"。
	 */
	struct htQueueDefinition *pxQueueSetContainer;
	UBaseType_t uxQueueSetMembers;
	uint8_t ucIsQueueSet;
#endif

} htQUEUE_t;

/** @brief 队列句柄。 */
typedef htQUEUE_t *QueueHandle_t;

/* ------------------------------------------------------------------ */
/* 队列创建与删除                                                       */
/* ------------------------------------------------------------------ */

/**
 * @brief 动态创建一个队列。
 *
 * @param[in] uxQueueLength 队列容量，必须大于 0。
 * @param[in] uxItemSize 每项字节数；为 0 时创建的是计数信号量。
 * @return 成功返回队列句柄，参数非法（容量为 0 或所需存储区溢出）或内存不足返回 NULL。
 * @note 存储区与控制块都从 htOS 堆上分配。
 */
QueueHandle_t htQueueCreate(UBaseType_t uxQueueLength, UBaseType_t uxItemSize);

/**
 * @brief 用调用方提供的存储区静态创建一个队列。
 *
 * @param[in] uxQueueLength 队列容量，必须大于 0。
 * @param[in] uxItemSize 每项字节数，为 0 时是计数信号量。
 * @param[in] pucQueueStorage 数据区，长度至少为 uxQueueLength * uxItemSize 字节。
 * @param[in] pxQueueBuffer 调用方提供的控制块。
 * @return 成功返回队列句柄，参数非法返回 NULL。
 * @note 静态队列删除时不会释放上述内存，生命周期由调用方管理。
 */
QueueHandle_t htQueueCreateStatic(
		UBaseType_t uxQueueLength, UBaseType_t uxItemSize, uint8_t *pucQueueStorage, htQUEUE_t *pxQueueBuffer);
/**
 * @brief 删除队列并释放其占用的资源。
 *
 * @param[in] xQueue 队列句柄。
 * @note 动态队列会一并释放存储区；静态队列只清零字段，不释放调用方内存。
 *       删除前必须保证没有任务还挂在它的等待链表上。
 */
void htQueueDelete(QueueHandle_t xQueue);

/* ------------------------------------------------------------------ */
/* 队列集（configUSE_QUEUE_SETS == 1 时可用）                           */
/* ------------------------------------------------------------------ */

#if (configUSE_QUEUE_SETS == 1)

/** @brief 队列集句柄：集合本身也是一个队列，元素是成员句柄。 */
typedef htQUEUE_t *QueueSetHandle_t;

/** @brief 队列集成员句柄：加入集合的队列或信号量。 */
typedef htQUEUE_t *QueueSetMemberHandle_t;

/**
 * @brief 创建一个队列集。
 *
 * @param[in] uxEventQueueLength 集合容量，必须不小于所有成员队列长度之和。
 * @return 成功返回集合句柄，容量为 0 或内存不足返回 NULL。
 *
 * @note 集合内部就是一个元素类型为 QueueSetMemberHandle_t 的普通队列：成员每次成功
 *       入队就往集合里投一个"我有数据"的令牌。取值只能靠 htQueueSelectFromSet()，
 *       直接把集合当队列接收会破坏令牌与成员数据的对应关系。
 */
QueueSetHandle_t htQueueSetCreate(UBaseType_t uxEventQueueLength);

/**
 * @brief 用调用方提供的存储区静态创建一个队列集（不申请堆）。
 *
 * @param[in] uxEventQueueLength 集合容量，应不小于所有成员队列长度之和。
 * @param[in] pucQueueStorage 令牌区，长度至少为 uxEventQueueLength * sizeof(QueueSetMemberHandle_t)。
 * @param[in] pxQueueSetBuffer 调用方提供的控制块。
 * @return 成功返回集合句柄；容量为 0 或参数为空返回 NULL。
 *
 * @note 与 htQueueCreateStatic() 对称，供 configTOTAL_HEAP_SIZE == 0 的纯静态固件使用；
 *       删除时不会释放这块内存。
 */
QueueSetHandle_t htQueueSetCreateStatic(
		UBaseType_t uxEventQueueLength, uint8_t *pucQueueStorage, htQUEUE_t *pxQueueSetBuffer);

/**
 * @brief 把一个队列或信号量加入集合。
 *
 * @param[in] xQueueOrSemaphore 待加入的成员，必须非空、非集合、尚未加入任何集合、且当前为空。
 * @param[in] xQueueSet 目标集合。
 * @return 加入成功返回 htPASS，参数非法或成员已属于某个集合返回 htFAIL。
 *
 * @note 成员必须为空，否则"加入前就有的数据"不会产生令牌，集合会漏报。
 * @warning 加入后删除成员前必须先 htQueueRemoveFromSet()，否则 htQueueDelete() 会拒绝。
 */
BaseType_t htQueueAddToSet(QueueSetMemberHandle_t xQueueOrSemaphore, QueueSetHandle_t xQueueSet);

/**
 * @brief 把成员从集合中摘出。
 *
 * @param[in] xQueueOrSemaphore 待摘除的成员。
 * @param[in] xQueueSet 它所属的集合。
 * @return 摘除成功返回 htPASS；参数非法或该成员不在此集合中返回 htFAIL。
 *
 * @note 摘除不会清掉集合里已经积累的令牌，那些令牌仍可能让 htQueueSelectFromSet()
 *       返回这个已摘除的成员。调用方应在摘除后自行 htQueueSelectFromSet() 把残留令牌
 *       抽干，或直接删除集合。
 */
BaseType_t htQueueRemoveFromSet(QueueSetMemberHandle_t xQueueOrSemaphore, QueueSetHandle_t xQueueSet);

/**
 * @brief 等待集合中任意一个成员有数据。
 *
 * @param[in] xQueueSet 集合句柄。
 * @param[in] xTicksToWait 一个成员都没有数据时的最长等待 tick 数；htBLOCKED_INDEFINITELY 表示永久等待。
 * @return 有数据的成员句柄；超时或参数非法返回 NULL。
 *
 * @note 返回的是"哪个成员有数据"，数据本身仍留在成员队列里，需要再对该成员调用
 *       htQueueReceive()/htSemaphoreTake() 取走。
 */
QueueSetMemberHandle_t htQueueSelectFromSet(QueueSetHandle_t xQueueSet, TickType_t xTicksToWait);

/**
 * @brief 中断上下文里取一个"有数据"的成员，不阻塞。
 *
 * @param[in] xQueueSet 集合句柄。
 * @return 有数据的成员句柄；集合为空或参数非法返回 NULL。
 * @note 不返回"是否唤醒了更高优先级任务"：集合只在成员入队时被填充，接收方在中断里
 *       取令牌不会唤醒任何发送者。
 */
QueueSetMemberHandle_t htQueueSelectFromSetFromISR(QueueSetHandle_t xQueueSet);

#endif /* configUSE_QUEUE_SETS */

/* ------------------------------------------------------------------ */
/* 队列发送                                                             */
/* ------------------------------------------------------------------ */

/**
 * @brief 向队列发送一个数据项，队列满时可阻塞等待。
 *
 * @param[in] xQueue 队列句柄。
 * @param[in] pvItemToQueue 待发送数据的指针，数据按 uxItemSize 复制进队列。
 * @param[in] xTicksToWait 队列满时最长阻塞的 tick 数，传 htBLOCKED_INDEFINITELY 表示永久等待。
 * @return htPASS(0) 表示成功，htFAIL(-1) 表示超时或参数非法。
 * @note 在中断里必须改用 htQueueSendFromISR()。
 */
BaseType_t htQueueSend(QueueHandle_t xQueue, const void *pvItemToQueue, TickType_t xTicksToWait);

/**
 * @brief 中断上下文里向队列发送一个数据项，不阻塞。
 *
 * @param[in] xQueue 队列句柄。
 * @param[in] pvItemToQueue 待发送数据的指针。
 * @param[out] pxHigherPriorityTaskWoken 若唤醒的任务优先级高于当前任务，会被写为 htTRUE。
 * @return htPASS(0) 表示成功，htFAIL(-1) 表示队列已满。
 * @note 调用本函数前应先将其置为 htFALSE，返回后据其值决定是否触发一次切换。
 */
BaseType_t htQueueSendFromISR(QueueHandle_t xQueue, const void *pvItemToQueue, BaseType_t *pxHigherPriorityTaskWoken);

/* ------------------------------------------------------------------ */
/* 队列接收                                                             */
/* ------------------------------------------------------------------ */

/**
 * @brief 从队列接收一个数据项，队列空时可阻塞等待。
 *
 * @param[in] xQueue 队列句柄。
 * @param[out] pvBuffer 接收缓冲区，长度至少为 uxItemSize 字节。
 * @param[in] xTicksToWait 队列空时最长阻塞的 tick 数，传 htBLOCKED_INDEFINITELY 表示永久等待。
 * @return htPASS(0) 表示成功，htFAIL(-1) 表示超时或参数非法。
 */
BaseType_t htQueueReceive(QueueHandle_t xQueue, void *pvBuffer, TickType_t xTicksToWait);

/**
 * @brief 中断上下文里从队列接收一个数据项，不阻塞。
 *
 * @param[in] xQueue 队列句柄。
 * @param[out] pvBuffer 接收缓冲区。
 * @param[out] pxHigherPriorityTaskWoken 若唤醒的任务优先级高于当前任务，会被写为 htTRUE。
 * @return htPASS(0) 表示成功，htFAIL(-1) 表示队列为空。
 */
BaseType_t htQueueReceiveFromISR(QueueHandle_t xQueue, void *pvBuffer, BaseType_t *pxHigherPriorityTaskWoken);

/* ------------------------------------------------------------------ */
/* 队列查询与维护                                                       */
/* ------------------------------------------------------------------ */

/**
 * @brief 读取队列中当前的消息数。
 *
 * @param[in] xQueue 队列句柄。
 * @return 当前消息数；句柄为 NULL 时返回 0。
 * @note 读取过程关中断，结果是一致的快照。
 */
UBaseType_t htQueueMessagesWaiting(QueueHandle_t xQueue);
/**
 * @brief 读取队列还剩多少空位。
 *
 * @param[in] xQueue 队列句柄。
 * @return 剩余空位数；句柄为 NULL 时返回 0。
 */
UBaseType_t htQueueSpacesAvailable(QueueHandle_t xQueue);
/**
 * @brief 覆盖写入，队列满时用新数据替换最旧的一项。
 *
 * @param[in] xQueue 队列句柄，必须是通过容量为 1、itemSize 非 0 创建的队列。
 * @param[in] pvItemToQueue 待写入数据的指针。
 * @return htPASS(0) 表示成功，htFAIL(-1) 表示队列形态不满足覆盖写条件。
 * @note 满时覆盖头部并重置读写指针，不会阻塞、也不会丢同步语义。
 */
BaseType_t htQueueOverwrite(QueueHandle_t xQueue, const void *pvItemToQueue);
/**
 * @brief 把队列恢复到刚创建时的空状态。
 *
 * @param[in] xQueue 队列句柄。
 * @return htPASS(0) 表示成功，htFAIL(-1) 表示句柄非法。
 * @note 会清零消息数、读写指针与收发锁；对信号量则把计数一并清零。
 */
BaseType_t htQueueReset(QueueHandle_t xQueue);
/**
 * @brief 查看队首数据但不出队，队列空时可阻塞等待。
 *
 * @param[in] xQueue 队列句柄。
 * @param[out] pvBuffer 接收缓冲区。
 * @param[in] xTicksToWait 最长阻塞 tick 数。
 * @return htPASS(0) 表示成功，htFAIL(-1) 表示超时或参数非法。
 * @note 数据仍留在队列里，仍会被后续的 htQueueReceive() 取走。
 */
BaseType_t htQueuePeek(QueueHandle_t xQueue, void *pvBuffer, TickType_t xTicksToWait);

#endif /* HT_QUEUE_H */
