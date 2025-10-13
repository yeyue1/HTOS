CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -I. -I../include
LDFLAGS = 

TEST_SOURCES = $(wildcard test_*.c)
TEST_TARGETS = $(TEST_SOURCES:.c=.out)

.PHONY: all test clean

all: $(TEST_TARGETS)

%.out: %.c unity.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test: all
	@echo "Running all tests..." > test_results.txt
	@for test in $(TEST_TARGETS); do \
		echo "\n=== Running $$test ===" | tee -a test_results.txt; \
		./$$test | tee -a test_results.txt; \
	done
	@echo "\nAll tests completed!" | tee -a test_results.txt

clean:
	rm -f $(TEST_TARGETS) test_results.txt *.o
