CC = cc
CFLAGS = -std=c11 -Wall -Wextra -Werror -pedantic -O2

COMMON_SRCS = bptree.c table.c sql.c
COMMON_OBJS = $(COMMON_SRCS:.c=.o)

all: main perf_test condition_perf_test unit_test

main: main.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ main.o $(COMMON_OBJS)

perf_test: perf_test.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ perf_test.o $(COMMON_OBJS)

condition_perf_test: condition_perf_test.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ condition_perf_test.o $(COMMON_OBJS)

unit_test: unit_test.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ unit_test.o $(COMMON_OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f *.o main perf_test condition_perf_test unit_test

.PHONY: all clean
