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
TEST_COMPOSITE = $(TEST_DIR)/test_composite
TEST_WAL = $(TEST_DIR)/test_wal

EXAMPLE_DIR = examples
EXAMPLE_C = $(EXAMPLE_DIR)/example_c

PREFIX ?= /usr/local
LIBDIR = $(PREFIX)/lib
INCLUDEDIR = $(PREFIX)/include

.PHONY: all clean test test_join test_perf test_edge test_composite test_wal example install

all: $(LIB) $(TEST_BASIC) $(TEST_JOIN) $(TEST_PERF) $(TEST_EDGE) $(TEST_COMPOSITE) $(TEST_WAL) example

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

$(TEST_COMPOSITE): $(TEST_DIR)/test_composite.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lmydb -Wl,-rpath,.

$(TEST_WAL): $(TEST_DIR)/test_wal.c $(LIB)
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

test_composite: $(TEST_COMPOSITE)
	@rm -f composite_data.bin composite_index.index composite_wal.bin
	LD_LIBRARY_PATH=. ./$(TEST_COMPOSITE)

test_wal: $(TEST_WAL)
	@rm -f wal_data.bin wal_index.index wal_test.bin wal_data2.bin wal_index2.index wal_test2.bin
	LD_LIBRARY_PATH=. ./$(TEST_WAL)

example: $(LIB)
	$(CC) $(CFLAGS) -o $(EXAMPLE_C) $(EXAMPLE_DIR)/example.c -L. -lmydb -Wl,-rpath,.
	@echo "C example built: $(EXAMPLE_C)"
	@echo "Lua example: $(EXAMPLE_DIR)/example.lua"
	@echo "Python example: $(EXAMPLE_DIR)/example.py"

install: $(LIB)
	install -d $(LIBDIR) $(INCLUDEDIR)
	install -m 755 $(LIB) $(LIBDIR)/
	install -m 644 include/mydb.h $(INCLUDEDIR)/
	@echo "Installed to $(PREFIX)"
	@echo "Library: $(LIBDIR)/$(LIB)"
	@echo "Header:  $(INCLUDEDIR)/mydb.h"

clean:
	rm -rf $(OBJ_DIR) $(LIB) $(TEST_BASIC) $(TEST_JOIN) $(TEST_PERF) $(TEST_EDGE) $(TEST_COMPOSITE) $(TEST_WAL) $(EXAMPLE_C) *.bin *.index
