# ---------------------------------------------------------------------------
# MiniDB build file
#
# Targets:
#   make            -> build the `minidb` interactive shell
#   make test       -> build and run the unit/integration test suite
#   make bench      -> build and run the benchmark harness
#   make clean      -> remove all build artefacts
#
# We use a plain Makefile (instead of CMake) because the toolchain only ships
# with make, and it keeps the build easy to read for the viva.
# ---------------------------------------------------------------------------

CXX      ?= c++
# -MMD -MP generate per-object .d files listing the headers each object depends
# on, so editing a header recompiles every affected .cpp. Without this, a stale
# object can be linked against a changed struct layout -> memory corruption.
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -g -O2 -Iinclude -pthread -MMD -MP
LDFLAGS  ?= -pthread

BUILD    := build

# Every .cpp under src/ is part of the engine, except main.cpp (the CLI entry)
# and bench_main.cpp (the benchmark entry), which provide their own main().
LIB_SRCS  := $(filter-out src/main.cpp src/bench_main.cpp,$(shell find src -name '*.cpp'))
LIB_OBJS  := $(patsubst src/%.cpp,$(BUILD)/%.o,$(LIB_SRCS))

TEST_SRCS := $(shell find tests -name '*.cpp')
TEST_OBJS := $(patsubst tests/%.cpp,$(BUILD)/tests/%.o,$(TEST_SRCS))

.PHONY: all test bench clean dirs

all: minidb

# --- engine static library -------------------------------------------------
$(BUILD)/libminidb.a: $(LIB_OBJS)
	@mkdir -p $(dir $@)
	ar rcs $@ $(LIB_OBJS)

# --- object files ----------------------------------------------------------
$(BUILD)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/tests/%.o: tests/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -Itests -c $< -o $@

# --- interactive shell -----------------------------------------------------
minidb: src/main.cpp $(BUILD)/libminidb.a
	$(CXX) $(CXXFLAGS) src/main.cpp -o $@ $(BUILD)/libminidb.a $(LDFLAGS)

# --- test runner -----------------------------------------------------------
$(BUILD)/test_runner: $(TEST_OBJS) $(BUILD)/libminidb.a
	$(CXX) $(CXXFLAGS) -Itests $(TEST_OBJS) -o $@ $(BUILD)/libminidb.a $(LDFLAGS)

test: $(BUILD)/test_runner
	@echo "=================== running test suite ==================="
	@./$(BUILD)/test_runner

# --- benchmark harness -----------------------------------------------------
bench: src/bench_main.cpp $(BUILD)/libminidb.a
	$(CXX) $(CXXFLAGS) src/bench_main.cpp -o $(BUILD)/bench $(BUILD)/libminidb.a $(LDFLAGS)
	@echo "=================== running benchmarks ==================="
	@./$(BUILD)/bench

# Extension benchmark: LSM-tree vs B+ tree/heap storage.
lsm-bench: benchmarks/lsm_benchmark.cpp $(BUILD)/libminidb.a
	$(CXX) $(CXXFLAGS) benchmarks/lsm_benchmark.cpp -o $(BUILD)/lsm_bench $(BUILD)/libminidb.a $(LDFLAGS)
	@echo "=============== running LSM vs B+Tree benchmark ==============="
	@./$(BUILD)/lsm_bench

clean:
	rm -rf $(BUILD) minidb minidb.d minidb.dSYM
	rm -rf data/*.db data/*.wal data/catalog.meta
	@echo "cleaned"

# Pull in auto-generated header dependencies (*.d) so header edits trigger
# rebuilds of every dependent translation unit.
-include $(LIB_OBJS:.o=.d)
-include $(TEST_OBJS:.o=.d)
