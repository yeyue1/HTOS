#ifndef UNITY_H
#define UNITY_H

#include <stdio.h>
#include <stdlib.h>

extern int Unity_NumTests;
extern int Unity_NumFailures;

#define TEST_ASSERT(condition)                                                                                         \
	do {                                                                                                               \
		Unity_NumTests++;                                                                                              \
		if (!(condition)) {                                                                                            \
			printf("FAIL: %s:%d - %s\n", __FILE__, __LINE__, #condition);                                              \
			Unity_NumFailures++;                                                                                       \
		}                                                                                                              \
	} while (0)

#define TEST_ASSERT_EQUAL(expected, actual)                                                                            \
	do {                                                                                                               \
		Unity_NumTests++;                                                                                              \
		if ((expected) != (actual)) {                                                                                  \
			printf("FAIL: %s:%d - Expected %d, got %d\n", __FILE__, __LINE__, (int)(expected), (int)(actual));         \
			Unity_NumFailures++;                                                                                       \
		}                                                                                                              \
	} while (0)

#define TEST_ASSERT_NULL(pointer) TEST_ASSERT((pointer) == NULL)
#define TEST_ASSERT_NOT_NULL(pointer) TEST_ASSERT((pointer) != NULL)

#define RUN_TEST(func)                                                                                                 \
	do {                                                                                                               \
		printf("Running %s...\n", #func);                                                                              \
		func();                                                                                                        \
	} while (0)

void UnityBegin(const char *filename);
int UnityEnd(void);

#endif /* UNITY_H */
