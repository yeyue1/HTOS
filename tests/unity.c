#include "unity.h"

int Unity_NumTests = 0;
int Unity_NumFailures = 0;

void UnityBegin(const char *filename)
{
	printf("\n========================================\n");
	printf("Testing: %s\n", filename);
	printf("========================================\n");
	Unity_NumTests = 0;
	Unity_NumFailures = 0;
}

int UnityEnd(void)
{
	printf("========================================\n");
	printf("Tests: %d, Failures: %d\n", Unity_NumTests, Unity_NumFailures);
	printf("========================================\n\n");
	return Unity_NumFailures;
}
