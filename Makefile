CC = gcc
CFLAGS = -Wall -Wextra -O2 -fPIC -I./include
LDFLAGS = -shared

SRC_DIR = src
OBJ_DIR = obj
TEST_DIR = tests

# 源文件
SOURCES = $(wildcard $(SRC_DIR)/utils/*.c) \
          $(wildcard $(SRC_DIR)/storage/*.c) \
          $(wildcard $(SRC_DIR)/core/*.c) \
          $(wildcard $(SRC_DIR)/types/*.c)

OBJECTS = $(SOURCES:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)

# 目标
LIB = libmydb.so
TEST_BASIC = $(TEST_DIR)/test_basic
TEST_JOIN = $(TEST_DIR)/test_join
TEST_PERF = $(TEST_DIR)/test_perf
TEST_EDGE = $(TEST_DIR)/test_edge

.PHONY: all clean test test_join test_perf test_edge

all: $(LIB) $(TEST_BASIC) $(TEST_JOIN) $(TEST_PERF) $(TEST_EDGE)

$(LIB): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $^

$(TEST_BASIC): $(TEST_DIR)/test_basic.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lmydb -Wl,-rpath,.

$(TEST_JOIN): $(TEST_DIR)/test_join.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lmydb -Wl,-rpath,.

$(TEST_PERF): $(TEST_DIR)/test_perf.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lmydb -Wl,-rpath,.

$(TEST_EDGE): $(TEST_DIR)/test_edge.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lmydb -Wl,-rpath,.

test: $(TEST_BASIC)
	@rm -f test_data.bin test_index.index test_wal.bin
	LD_LIBRARY_PATH=. ./$(TEST_BASIC)

test_join: $(TEST_JOIN)
	@rm -f join_data.bin join_index.index join_wal.bin
	LD_LIBRARY_PATH=. ./$(TEST_JOIN)

test_perf: $(TEST_PERF)
	@rm -f perf_data.bin perf_index.index perf_wal.bin
	LD_LIBRARY_PATH=. ./$(TEST_PERF)

test_edge: $(TEST_EDGE)
	@rm -f edge_data.bin edge_index.index edge_wal.bin
	LD_LIBRARY_PATH=. ./$(TEST_EDGE)

clean:
	rm -rf $(OBJ_DIR) $(LIB) $(TEST_BASIC) $(TEST_JOIN) $(TEST_PERF) $(TEST_EDGE) *.bin *.index
