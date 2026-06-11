CC       ?= gcc
CFLAGS   ?= -Wall -Wextra -Wpedantic -std=c11 -O2 -g
CFLAGS   += -Iinclude -D_GNU_SOURCE
LDFLAGS  ?=
LIBS     ?= -lrt

SRC_DIR   = src
INC_DIR   = include
BUILD_DIR = build

CORE_SRCS = $(filter-out $(SRC_DIR)/deploy.c $(SRC_DIR)/clusterctl.c, $(wildcard $(SRC_DIR)/*.c))
OBJS = $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(CORE_SRCS))
DEPS = $(OBJS:.o=.d)
# Tool object files built separately
TOOL_SRCS = $(SRC_DIR)/deploy.c $(SRC_DIR)/clusterctl.c

TARGET    = agent
TOOLS     = $(BUILD_DIR)/deploy $(BUILD_DIR)/clusterctl

.PHONY: all tools clean test

all: $(BUILD_DIR) $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)
	@echo "  BUILD  $@"

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -MMD -c -o $@ $<
	@echo "  CC     $<"

-include $(DEPS)

tools: $(BUILD_DIR) $(TOOLS)

$(BUILD_DIR)/deploy: $(BUILD_DIR)/deploy.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD_DIR)/clusterctl: $(BUILD_DIR)/clusterctl.o $(BUILD_DIR)/protocol.o $(BUILD_DIR)/json.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)


clean:
	rm -rf $(BUILD_DIR) $(TARGET) $(TOOLS)
	@echo "  CLEAN"

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/
	@echo "  INSTALL $(TARGET)"

.PHONY: all tools clean test install

# Test targets
TEST_DIR = tests
TEST_BINS = tests/test_election tests/test_protocol tests/test_json

tests/test_election: tests/test_election.c src/election.c
	  -o  $^ 

tests/test_protocol: tests/test_protocol.c src/protocol.c src/json.c
	  -o  $^ 

tests/test_json: tests/test_json.c src/json.c
	  -o  $^ 

test:  
	for t in ; do ./190561t || exit 1; done
.PHONY: test


# Test targets
TEST_DIR = tests
TEST_BINS = tests/test_election tests/test_protocol tests/test_json

tests/test_election: tests/test_election.c src/election.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

tests/test_protocol: tests/test_protocol.c src/protocol.c src/json.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

tests/test_json: tests/test_json.c src/json.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test: $(TEST_DIR) $(TEST_BINS)
	for t in $(TEST_BINS); do ./$$t || exit 1; done
.PHONY: test
