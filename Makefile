CC = gcc

ifeq ($(OS),Windows_NT)
THREAD_FLAGS =
POSIX_DEFS =
SOCKET_LIBS = -lws2_32
BIN_EXT = .exe
else
THREAD_FLAGS = -pthread
POSIX_DEFS = -D_XOPEN_SOURCE=700
SOCKET_LIBS =
BIN_EXT =
endif

CFLAGS = -std=c11 -Wall -Wextra -Werror -pedantic -O2 $(POSIX_DEFS) $(THREAD_FLAGS) -Isrc/core -Isrc/server

BUILD_DIR = build
BIN_DIR = $(BUILD_DIR)/bin
OBJ_DIR = $(BUILD_DIR)/obj

CORE_SRCS = src/core/bptree.c src/core/table.c src/core/sql.c
SERVER_LIB_SRCS = src/server/db_server.c src/server/api.c src/server/platform.c
SERVER_SRCS = src/server/server.c src/server/http_server.c $(SERVER_LIB_SRCS)
MAIN_SRCS = src/cli/main.c

UNIT_TEST_SRCS = tests/unit/unit_test.c
PERF_TEST_SRCS = benchmarks/perf_test.c
CONDITION_PERF_TEST_SRCS = benchmarks/condition_perf_test.c
PERF10_SRCS = benchmarks/perf10.c
COND10_SRCS = benchmarks/cond10.c

CORE_OBJS = $(patsubst %.c,$(OBJ_DIR)/%.o,$(CORE_SRCS))
SERVER_LIB_OBJS = $(patsubst %.c,$(OBJ_DIR)/%.o,$(SERVER_LIB_SRCS))
SERVER_OBJS = $(patsubst %.c,$(OBJ_DIR)/%.o,$(SERVER_SRCS))
MAIN_OBJS = $(patsubst %.c,$(OBJ_DIR)/%.o,$(MAIN_SRCS))

UNIT_TEST_OBJS = $(patsubst %.c,$(OBJ_DIR)/%.o,$(UNIT_TEST_SRCS))
PERF_TEST_OBJS = $(patsubst %.c,$(OBJ_DIR)/%.o,$(PERF_TEST_SRCS))
CONDITION_PERF_TEST_OBJS = $(patsubst %.c,$(OBJ_DIR)/%.o,$(CONDITION_PERF_TEST_SRCS))
PERF10_OBJS = $(patsubst %.c,$(OBJ_DIR)/%.o,$(PERF10_SRCS))
COND10_OBJS = $(patsubst %.c,$(OBJ_DIR)/%.o,$(COND10_SRCS))

MAIN_BIN = $(BIN_DIR)/main$(BIN_EXT)
PERF_TEST_BIN = $(BIN_DIR)/perf_test$(BIN_EXT)
CONDITION_PERF_TEST_BIN = $(BIN_DIR)/condition_perf_test$(BIN_EXT)
UNIT_TEST_BIN = $(BIN_DIR)/unit_test$(BIN_EXT)
SERVER_BIN = $(BIN_DIR)/server$(BIN_EXT)
PERF10_BIN = $(BIN_DIR)/perf10$(BIN_EXT)
COND10_BIN = $(BIN_DIR)/cond10$(BIN_EXT)

all: main perf_test condition_perf_test unit_test server

main: $(MAIN_BIN)

perf_test: $(PERF_TEST_BIN)

condition_perf_test: $(CONDITION_PERF_TEST_BIN)

unit_test: $(UNIT_TEST_BIN)

server: $(SERVER_BIN)

benchmarks: perf10 cond10

perf10: $(PERF10_BIN)

cond10: $(COND10_BIN)

$(MAIN_BIN): $(MAIN_OBJS) $(CORE_OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^

$(PERF_TEST_BIN): $(PERF_TEST_OBJS) $(CORE_OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^

$(CONDITION_PERF_TEST_BIN): $(CONDITION_PERF_TEST_OBJS) $(CORE_OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^

$(UNIT_TEST_BIN): $(UNIT_TEST_OBJS) $(SERVER_LIB_OBJS) $(CORE_OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(THREAD_FLAGS)

$(SERVER_BIN): $(SERVER_OBJS) $(CORE_OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(SOCKET_LIBS)

$(PERF10_BIN): $(PERF10_OBJS) $(CORE_OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^

$(COND10_BIN): $(COND10_OBJS) $(CORE_OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^

$(OBJ_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN_DIR):
	@mkdir -p $@

clean:
	rm -rf $(BUILD_DIR)
	rm -f *.o main$(BIN_EXT) perf_test$(BIN_EXT) condition_perf_test$(BIN_EXT) unit_test$(BIN_EXT) server$(BIN_EXT)

.PHONY: all clean main perf_test condition_perf_test unit_test server benchmarks perf10 cond10
