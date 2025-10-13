# HTOS 单元测试

本目录包含 HTOS 项目的单元测试。

## 目录结构

```
tests/
├── unity.h          # Unity 测试框架头文件
├── unity.c          # Unity 测试框架实现
├── test_example.c   # 示例测试文件
├── Makefile         # 编译和运行脚本
└── README.md        # 本文档
```

## 使用方法

### 本地运行测试

```bash
cd tests
make all      # 编译所有测试
make test     # 运行所有测试
make clean    # 清理编译产物
```

### 添加新测试

1. 创建新的测试文件 `test_yourmodule.c`
2. 包含 `unity.h` 头文件
3. 编写测试用例函数
4. 在 `main()` 函数中使用 `RUN_TEST()` 运行测试
5. 运行 `make test` 验证

### 测试用例示例

```c
#include "unity.h"

void test_your_function(void) {
    TEST_ASSERT_EQUAL(expected, actual);
    TEST_ASSERT(condition);
    TEST_ASSERT_NULL(pointer);
}

int main(void) {
    UnityBegin("test_yourmodule.c");
    RUN_TEST(test_your_function);
    return UnityEnd();
}
```

## CI/CD 集成

测试会在 GitHub Actions 中自动运行，参见 `.github/workflows/main.yaml`。
