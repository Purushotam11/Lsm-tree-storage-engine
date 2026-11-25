# Makefile for lsm-tree.
#
# Targets:
#   make            -- build demo + test_lsm + bench (release flags)
#   make test       -- run the test suite
#   make demo       -- run the scripted demo
#   make bench      -- run the throughput / latency benchmark
#   make clean

CXX      ?= g++
CXXFLAGS ?= -std=c++20 -O3 -Wall -Wextra -Wpedantic -Isrc
LDFLAGS  ?=

SRC_DIR   := src
TEST_DIR  := tests
BENCH_DIR := bench
BUILD_DIR := build

LSM_OBJ   := $(BUILD_DIR)/lsm.o
DEMO_BIN  := $(BUILD_DIR)/demo
TEST_BIN  := $(BUILD_DIR)/test_lsm
BENCH_BIN := $(BUILD_DIR)/bench

.PHONY: all test demo bench clean

all: $(DEMO_BIN) $(TEST_BIN) $(BENCH_BIN)

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

$(LSM_OBJ): $(SRC_DIR)/lsm.cpp $(SRC_DIR)/lsm.hpp \
            $(SRC_DIR)/memtable.hpp $(SRC_DIR)/sstable.hpp \
            $(SRC_DIR)/wal.hpp $(SRC_DIR)/bloom.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $(SRC_DIR)/lsm.cpp -o $@

$(DEMO_BIN): $(SRC_DIR)/demo.cpp $(LSM_OBJ) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(SRC_DIR)/demo.cpp $(LSM_OBJ) -o $@ $(LDFLAGS)

$(TEST_BIN): $(TEST_DIR)/test_lsm.cpp $(LSM_OBJ) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(TEST_DIR)/test_lsm.cpp $(LSM_OBJ) -o $@ $(LDFLAGS)

$(BENCH_BIN): $(BENCH_DIR)/bench.cpp $(LSM_OBJ) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(BENCH_DIR)/bench.cpp $(LSM_OBJ) -o $@ $(LDFLAGS)

test: $(TEST_BIN)
	@./$(TEST_BIN)

demo: $(DEMO_BIN)
	@./$(DEMO_BIN)

bench: $(BENCH_BIN)
	@./$(BENCH_BIN)

clean:
	@rm -rf $(BUILD_DIR)
