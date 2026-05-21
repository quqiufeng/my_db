CC = gcc
CFLAGS = -Wall -Wextra -O2 -fPIC -I./include
LDFLAGS = -shared


# Use GPU ONNX Runtime (RTX 3080 CUDA support)
# GPU library copied from anaconda env
ONNX_CFLAGS = -I/opt/piper-src/build/p/src/piper_phonemize_external/lib/onnxruntime-linux-x64-1.14.1/include
ONNX_LIB = .
ONNX_LDFLAGS = -L$(ONNX_LIB) -lonnxruntime_gpu -Wl,-rpath,$(ONNX_LIB)

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
TEST_HNSW = $(TEST_DIR)/test_hnsw

EXAMPLE_DIR = examples
EXAMPLE_C = $(EXAMPLE_DIR)/example_c
TOOLS_DIR = tools
IMPORT_BOOK = $(TOOLS_DIR)/import_book
CACHE_SERVER = $(TOOLS_DIR)/cache_server
CACHE_HTTP_SERVER = $(TOOLS_DIR)/cache_http_server
CACHE_SNAPSHOT = $(TOOLS_DIR)/cache_snapshot
VECTOR_GENERATOR = $(TOOLS_DIR)/vector_generator
VECTOR_SEARCH = $(TOOLS_DIR)/vector_search

PREFIX ?= /usr/local
LIBDIR = $(PREFIX)/lib
INCLUDEDIR = $(PREFIX)/include

.PHONY: all clean test test_join test_perf test_edge test_composite test_wal test_cache test_cache_full test_http test_hnsw example install

all: $(LIB) $(ONNX_EMBEDDER_LIB) $(VECTOR_ENGINE_LIB) $(TEST_BASIC) $(TEST_JOIN) $(TEST_PERF) $(TEST_EDGE) $(TEST_COMPOSITE) $(TEST_WAL) $(TEST_CACHE) $(TEST_CACHE_FULL) $(TEST_HTTP_SERVER) $(TEST_HNSW) $(IMPORT_BOOK) $(CACHE_SERVER) $(CACHE_HTTP_SERVER) $(CACHE_SNAPSHOT) $(VECTOR_GENERATOR) $(VECTOR_SEARCH) $(BUILD_HNSW_INDEX) $(CODE_INDEXER) $(BATCH_EMBEDDER) $(CACHE_IMPORT) $(CACHE_QUERY) example

$(LIB): $(OBJECTS)
	$(CC) -shared -fopenmp -o $@ $^ -lm

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fopenmp -c -o $@ $^

$(OBJ_DIR)/cache/hnsw.o: $(SRC_DIR)/cache/hnsw.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fopenmp -c -o $@ $^

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

test_hnsw: $(TEST_HNSW)
	@rm -rf ./test_hnsw_cache
	LD_LIBRARY_PATH=. ./$(TEST_HNSW)

$(TEST_CACHE): $(TEST_DIR)/test_cache.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lmydb -Wl,-rpath,.

$(TEST_CACHE_FULL): $(TEST_DIR)/test_cache_full.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lmydb -Wl,-rpath,.

$(TEST_HTTP_SERVER): $(TEST_DIR)/test_http_server.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lmydb -Wl,-rpath,.

$(TEST_HNSW): $(TEST_DIR)/test_hnsw.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lmydb -lm -Wl,-rpath,.

$(IMPORT_BOOK): $(TOOLS_DIR)/import_book.c $(SRC_DIR)/embedding/onnx_embedder.c $(LIB)
	$(CC) $(CFLAGS) $(ONNX_CFLAGS) -I./include/tokenizers-cpp -o $@ $(TOOLS_DIR)/import_book.c $(SRC_DIR)/embedding/onnx_embedder.c $(TOKENIZERS_CPP_LIBS) -L. -lmydb $(ONNX_LDFLAGS) -lm -lstdc++ -Wl,-rpath,.

$(CACHE_SERVER): $(TOOLS_DIR)/cache_server.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lmydb -Wl,-rpath,.

$(CACHE_HTTP_SERVER): $(TOOLS_DIR)/cache_http_server.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lmydb -Wl,-rpath,.

$(CACHE_SNAPSHOT): $(TOOLS_DIR)/cache_snapshot.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lmydb -Wl,-rpath,.

CODE_INDEXER = $(TOOLS_DIR)/code_indexer
BATCH_EMBEDDER = $(TOOLS_DIR)/batch_embedder
CACHE_IMPORT = $(TOOLS_DIR)/cache_import
CACHE_QUERY = $(TOOLS_DIR)/cache_query

# Vector engine library
VECTOR_ENGINE_OBJ = $(OBJ_DIR)/vector_engine.o
VECTOR_ENGINE_LIB = libvector_engine.so

$(VECTOR_ENGINE_OBJ): $(SRC_DIR)/vector_engine.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(ONNX_CFLAGS) -c -o $@ $(SRC_DIR)/vector_engine.c

$(VECTOR_ENGINE_LIB): $(VECTOR_ENGINE_OBJ) $(ONNX_EMBEDDER_LIB) $(LIB)
	$(CC) -shared -o $@ $< -L. -lmydb -lonnx_embedder $(ONNX_LDFLAGS) -lm -ldl -ljansson -Wl,-rpath,'$$ORIGIN/..'

$(CACHE_IMPORT): $(TOOLS_DIR)/cache_import.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $(TOOLS_DIR)/cache_import.c -L. -lmydb -ljansson -Wl,-rpath,.

$(CACHE_QUERY): $(TOOLS_DIR)/cache_query.c $(LIB) $(VECTOR_ENGINE_LIB)
	$(CC) $(CFLAGS) $(ONNX_CFLAGS) -fopenmp -o $@ $(TOOLS_DIR)/cache_query.c -L. -lmydb -lvector_engine -lonnx_embedder $(ONNX_LDFLAGS) -lm -ldl -ljansson -Wl,-rpath,'$$ORIGIN/..'

BUILD_HNSW_INDEX = $(TOOLS_DIR)/build_hnsw_index

$(BUILD_HNSW_INDEX): $(TOOLS_DIR)/build_hnsw_index.c $(LIB)
	$(CC) $(CFLAGS) -fopenmp -o $@ $< -L. -lmydb -lm -Wl,-rpath,.

$(VECTOR_GENERATOR): $(TOOLS_DIR)/vector_generator.c $(ONNX_EMBEDDER_LIB) $(LIB)
	$(CC) $(CFLAGS) $(ONNX_CFLAGS) -o $@ $(TOOLS_DIR)/vector_generator.c -L. -lmydb -lonnx_embedder $(ONNX_LDFLAGS) -lm -ldl -Wl,-rpath,'$$ORIGIN/..'

$(VECTOR_SEARCH): $(TOOLS_DIR)/vector_search.c $(VECTOR_ENGINE_LIB)
	$(CC) $(CFLAGS) $(ONNX_CFLAGS) -o $@ $(TOOLS_DIR)/vector_search.c -L. -lvector_engine -lmydb -lonnx_embedder $(ONNX_LDFLAGS) -lm -ldl -ljansson -Wl,-rpath,'$$ORIGIN/..'

# ONNX Embedder shared library (for Python FFI)
ONNX_EMBEDDER_OBJ = $(OBJ_DIR)/embedding/onnx_embedder.o
ONNX_EMBEDDER_LIB = libonnx_embedder.so

$(CODE_INDEXER): $(TOOLS_DIR)/code_indexer.c
	$(CC) $(CFLAGS) -o $@ $< -lm

$(BATCH_EMBEDDER): $(TOOLS_DIR)/batch_embedder.c $(ONNX_EMBEDDER_LIB)
	$(CC) $(CFLAGS) $(ONNX_CFLAGS) -o $@ $(TOOLS_DIR)/batch_embedder.c -L. -lonnx_embedder $(ONNX_LDFLAGS) -lm -ldl -Wl,-rpath,'$$ORIGIN/..'

# tokenizers-cpp libraries
TOKENIZERS_CPP_DIR = lib/tokenizers-cpp
TOKENIZERS_CPP_LIBS = $(TOKENIZERS_CPP_DIR)/libtokenizers_cpp.a \
                      $(TOKENIZERS_CPP_DIR)/libtokenizers_c.a \
                      $(TOKENIZERS_CPP_DIR)/libonig.a

$(ONNX_EMBEDDER_OBJ): $(SRC_DIR)/embedding/onnx_embedder.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(ONNX_CFLAGS) -I./include/tokenizers-cpp -c -o $@ $(SRC_DIR)/embedding/onnx_embedder.c

$(ONNX_EMBEDDER_LIB): $(ONNX_EMBEDDER_OBJ)
	$(CC) -shared -o $@ $< $(TOKENIZERS_CPP_LIBS) $(ONNX_LDFLAGS) -lm -ldl -lstdc++

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
	rm -rf $(OBJ_DIR) $(LIB) $(ONNX_EMBEDDER_LIB) $(VECTOR_ENGINE_LIB) $(TEST_BASIC) $(TEST_JOIN) $(TEST_PERF) $(TEST_EDGE) $(TEST_COMPOSITE) $(TEST_WAL) $(TEST_CACHE) $(TEST_CACHE_FULL) $(TEST_HTTP_SERVER) $(TEST_HNSW) $(IMPORT_BOOK) $(CACHE_HTTP_SERVER) $(CACHE_SNAPSHOT) $(VECTOR_GENERATOR) $(VECTOR_SEARCH) $(BUILD_HNSW_INDEX) $(CODE_INDEXER) $(BATCH_EMBEDDER) $(CACHE_IMPORT) $(CACHE_QUERY) $(EXAMPLE_C) tests/test_codebert_embedder tests/bench_codebert *.bin *.index test_cache_dir test_cache_lru
