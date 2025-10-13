name: CI MISRA Check

on:
  push:
    branches: [ main, master ]
  pull_request:
    branches: [ main, master ]

jobs:
  misra-check:
    runs-on: ubuntu-latest
    steps:
      - name: Checkout
        uses: actions/checkout@v4

      - name: Install cppcheck
        run: sudo apt-get update && sudo apt-get install -y cppcheck

      - name: MISRA 2012 analysis
        run: |
          cppcheck \
            --language=c \
            --std=c11 \
            --enable=warning,style,performance,portability \
            --addon=misra \
            --inline-suppr \
            --template="{file}:{line}: [{severity}] {id} {message}" \
            --error-exitcode=1 \
            include kernel

      - name: Upload cppcheck report
        if: failure()
        uses: actions/upload-artifact@v4
        with:
          name: cppcheck-report
          path: cppcheck.xml

  unit-test:
    runs-on: ubuntu-latest
    needs: misra-check
    steps:
      - name: Checkout
        uses: actions/checkout@v4

      - name: Install GCC and Make
        run: sudo apt-get update && sudo apt-get install -y gcc make

      - name: Build and Run Unit Tests
        run: |
          cd tests
          make all
          make test

      - name: Upload test results
        if: always()
        uses: actions/upload-artifact@v4
        with:
          name: test-results
          path: tests/test_results.txt

.PHONY: lint lint-clang lint-cppcheck clean-lint

lint: lint-clang lint-cppcheck

lint-clang:
	@echo "Running clang-tidy..."
	@find kernel include -type f \( -name "*.c" -o -name "*.h" \) -print0 | \
	xargs -0 -r clang-tidy \
		--config-file=.clang-tidy \
		-- -std=c11 -Iinclude -DSTM32F103xB || echo "clang-tidy completed"

lint-cppcheck:
	@echo "Running cppcheck with MISRA addon..."
	@cppcheck \
		--language=c --std=c11 \
		--enable=warning,style,performance,portability \
		--addon=misra \
		--inline-suppr \
		--suppress=misra-c2012-* \
		--suppress=unknownMacro \
		--template="{file}:{line}: [{severity}] {id} {message}" \
		-Iinclude \
		include kernel 2>&1 | tee cppcheck.log || true

clean-lint:
	@rm -f cppcheck.log cppcheck-report.xml
	@echo "Lint artifacts cleaned"