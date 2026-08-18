# HTOS 单元测试

本目录包含 HTOS 项目的单元测试。

## 目录结构

`run_tests.py` 是 Windows/Linux 共用的唯一测试入口，会以 `-Werror` 编译并运行列表、队列和堆分配器的主机回归测试。

## 使用方法

### 本地运行测试

```bash
cd tests
python run_tests.py --build        # 编译并运行全部测试
python run_tests.py --build --cc clang
python run_tests.py --clean        # 清理编译产物
```

也可继续使用 `make test` / `make clean`。从仓库根目录运行统一构建 profile 时，使用 `codex_build.toml` 中的 `host_tests_ai_guard`。

### 添加新测试

1. 创建新的测试文件 `test_yourmodule.c`。
2. 在 `run_tests.py` 的 `TEST_SPECS` 中声明测试与被测源文件。
3. 包含 `unity.h` 并使用 `RUN_TEST()` 注册用例。
4. 运行 `python tests/run_tests.py --build` 验证。

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

测试会在 GitHub Actions 中自动运行，参见 `.github/workflows/main.yaml`：

- Linux GCC：严格警告门禁。
- Linux Clang：AddressSanitizer + UndefinedBehaviorSanitizer。
- Windows Clang：验证测试入口和代码的跨平台性。
- clang-format、clang-tidy、cppcheck：失败时直接阻断 CI。

CI 还会使用 `tests/stubs` 中的最小 CMSIS 边界对 Cortex-M 任务与调度器代码做语法和警告检查。
这些 stub 只用于编译门禁，不模拟硬件；仓库目前未包含完整的厂商 HAL 和板级链接工程。
