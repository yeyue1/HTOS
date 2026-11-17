#ifndef HT_DEBUG_H
#define HT_DEBUG_H

#include "htconfig.h"

#if configENABLE_DEBUG_LOG
#include <stdio.h>
#define HTOS_DEBUG_LOG(...) printf(__VA_ARGS__)
#else
#define HTOS_DEBUG_LOG(...) ((void)0)
#endif

#endif /* HT_DEBUG_H */
