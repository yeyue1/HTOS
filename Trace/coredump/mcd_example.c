/*
 * Copyright (c) 2025, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2025-08-16     Yeyue       first version
 */

#include "coredump.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>


typedef void (*fault_func)(float);

/* wrapper: 导出给链接器期望的符号名 */
int mcd_test_htos(const char *command); /* 这个在 htosAdaptation.c 中实现 */

/* 保持原有的 mcd_test 实现用于兼容性 */
static int mcd_test(int argc, char **argv) {
  if (argc < 2) {
    printf("Please input 'mcd_test <COREDUMP|ASSERT|FAULT>' \n");
    return 0;
  }

  /* 直接调用新的简化接口 */
  if (strcmp(argv[1], "COREDUMP") == 0) {
    return mcd_test_htos("SERIAL");
  } else if (strcmp(argv[1], "ASSERT") == 0) {
    return mcd_test_htos("ASSERT");
  } else if (strcmp(argv[1], "FAULT") == 0) {
    return mcd_test_htos("FAULT");
  }

  return 0;
}

MCD_CMD_EXPORT(mcd_test, mCoreDump test : mcd_test<COREDUMP | ASSERT | FAULT>);