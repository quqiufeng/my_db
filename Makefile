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
          $(wildcard $(SRC_DIR)/types/*.c) \
          $(wildcard $(SRC_DIR)/cache/*.c)

OBJECTS = $(SOURCES:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)

# 目标
LIB = libmydb.so
TEST_BASIC = $(TEST_DIR)/test_basic
TEST_JOIN = $(TEST_DIR)/test_join
TEST_PERF = $(TEST_DIR)/test_perf
TEST_EDGE = $(TEST_DIR)/test_edge
TEST_COMPOSITE = $(TEST_DIR)/test_composite
TEST_WAL = $(TEST_DIR)/test_wal
TEST_CACHE = $(TEST_DIR)/test_cache
TEST_CACHE_FULL = $(TEST_DIR)/test_cache_full
TEST_HTTP_SERVER = $(TEST_DIR)/test_http_server

EXAMPLE_DIR = examples
EXAMPLE_C = $(EXAMPLE_DIR)/example_c
TOOLS_DIR = tools
IMPORT_BOOK = $(TOOLS_DIR)/import_book
CACHE_SERVER = $(TOOLS_DIR)/cache_server
CACHE_HTTP_SERVER = $(TOOLS_DIR)/cache_http_server

PREFIX ?= /usr/local
LIBDIR = $(PREFIX)/lib
INCLUDEDIR = $(PREFIX)/include

.PHONY: all clean test test_join test_perf test_edge test_composite test_wal test_cache test_cache_full test_http example install

all: $(LIB) $(TEST_BASIC) $(TEST_JOIN) $(TEST_PERF) $(TEST_EDGE) $(TEST_COMPOSITE) $(TEST_WAL) $(TEST_CACHE) $(TEST_CACHE_FULL) $(TEST_HTTP_SERVER) $(IMPORT_BOOK) $(CACHE_SERVER) $(CACHE_HTTP_SERVER) example

$(LIB): $(OBJECTS)
	$(CC) -shared -o $@ $^ -lm

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

test_cache: $(TEST_CACHE)
	@rm -rf test_cache_dir test_cache_lru
	LD_LIBRARY_PATH=. ./$(TEST_CACHE)

test_cache_full: $(TEST_CACHE_FULL)
	@rm -rf /tmp/test_crud /tmp/test_ns /tmp/test_search /tmp/test_ttl /tmp/test_iter /tmp/test_persist /tmp/test_stats
	LD_LIBRARY_PATH=. ./$(TEST_CACHE_FULL)

test_http: $(TEST_HTTP_SERVER) $(CACHE_HTTP_SERVER)
	@rm -rf test_http_cache
	LD_LIBRARY_PATH=. ./$(TEST_HTTP_SERVER)

$(TEST_CACHE): $(TEST_DIR)/test_cache.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lmydb -Wl,-rpath,.

$(TEST_CACHE_FULL): $(TEST_DIR)/test_cache_full.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lmydb -Wl,-rpath,.

$(TEST_HTTP_SERVER): $(TEST_DIR)/test_http_server.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lmydb -Wl,-rpath,.

$(IMPORT_BOOK): $(TOOLS_DIR)/import_book.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lmydb -Wl,-rpath,.

$(CACHE_SERVER): $(TOOLS_DIR)/cache_server.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lmydb -Wl,-rpath,.

$(CACHE_HTTP_SERVER): $(TOOLS_DIR)/cache_http_server.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lmydb -Wl,-rpath,.

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
	rm -rf $(OBJ_DIR) $(LIB) $(TEST_BASIC) $(TEST_JOIN) $(TEST_PERF) $(TEST_EDGE) $(TEST_COMPOSITE) $(TEST_WAL) $(TEST_CACHE) $(TEST_CACHE_FULL) $(TEST_HTTP_SERVER) $(IMPORT_BOOK) $(CACHE_HTTP_SERVER) $(EXAMPLE_C) *.bin *.index test_cache_dir test_cache_lru
