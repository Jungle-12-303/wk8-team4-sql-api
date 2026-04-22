CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Werror -pedantic -O2

COMMON_SRCS = bptree.c table.c sql.c
COMMON_OBJS = $(COMMON_SRCS:.c=.o)
DB_SERVER_OBJS = db_server.o
SERVER_SRCS = server.c db_server.c
SERVER_OBJS = $(SERVER_SRCS:.c=.o)

all: main perf_test condition_perf_test unit_test server

main: main.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ main.o $(COMMON_OBJS)

perf_test: perf_test.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ perf_test.o $(COMMON_OBJS)

condition_perf_test: condition_perf_test.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ condition_perf_test.o $(COMMON_OBJS)

unit_test: unit_test.o $(DB_SERVER_OBJS) $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ unit_test.o $(DB_SERVER_OBJS) $(COMMON_OBJS)

server: $(SERVER_OBJS) $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $(SERVER_OBJS) $(COMMON_OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f *.o main perf_test condition_perf_test unit_test server

.PHONY: all clean
