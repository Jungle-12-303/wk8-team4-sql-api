CC = gcc

ifeq ($(OS),Windows_NT)
THREAD_FLAGS =
SOCKET_LIBS = -lws2_32
BIN_EXT = .exe
else
THREAD_FLAGS = -pthread
SOCKET_LIBS =
BIN_EXT =
endif

CFLAGS = -std=c11 -Wall -Wextra -Werror -pedantic -O2 $(THREAD_FLAGS)

COMMON_SRCS = bptree.c table.c sql.c
COMMON_OBJS = $(COMMON_SRCS:.c=.o)
UTILITY_SRCS = db_server.c api.c platform.c
UTILITY_OBJS = $(UTILITY_SRCS:.c=.o)
DB_SERVER_OBJS = db_server.o api.o platform.o
SERVER_SRCS = server.c http_server.c $(UTILITY_SRCS)
SERVER_OBJS = $(SERVER_SRCS:.c=.o)

all: main perf_test condition_perf_test unit_test server

main: main.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ main.o $(COMMON_OBJS)

perf_test: perf_test.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ perf_test.o $(COMMON_OBJS)

condition_perf_test: condition_perf_test.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ condition_perf_test.o $(COMMON_OBJS)

unit_test: unit_test.o $(DB_SERVER_OBJS) $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ unit_test.o $(DB_SERVER_OBJS) $(COMMON_OBJS) $(THREAD_FLAGS)

server: $(SERVER_OBJS) $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $(SERVER_OBJS) $(COMMON_OBJS) $(SOCKET_LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f *.o main$(BIN_EXT) perf_test$(BIN_EXT) condition_perf_test$(BIN_EXT) unit_test$(BIN_EXT) server$(BIN_EXT)

.PHONY: all clean
