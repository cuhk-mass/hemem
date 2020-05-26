CC = gcc
CFLAGS = -g -Wall -O2 
#CFLAGS = -g3 -Wall -O0
INCLUDES = -I/root/hmem/linux/usr/include
LIBS = -lm -lpthread

default: all

all: gups-simple gups-lru gups-lru-swap test-simple test-lru gups test

gups: gups.o
	$(CC) $(CFLAGS) $(INCLUDES) -o gups gups.o zipf.o $(LIBS) -L../ -lhemem

gups-simple: gups.o
	$(CC) $(CFLAGS) $(INCLUDES) -o gups-simple gups.o zipf.o $(LIBS) -L../ -lhemem-simple

gups-lru: gups.o
	$(CC) $(CFLAGS) $(INCLUDES) -o gups-lru gups.o zipf.o $(LIBS) -L../ -lhemem-lru

gups-lru-swap: gups.o
	$(CC) $(CFLAGS) $(INCLUDES) -o gups-lru-swap gups.o zipf.o $(LIBS) -L../ -lhemem-lru-swap

test: test.o
	$(CC) $(CFLAGS) $(INCLUDES) -o test test.o $(LIBS) -L../ -lhemem

test-simple: test.o
	$(CC) $(CFLAGS) $(INCLUDES) -o test-simple test.o $(LIBS) -L../ -lhemem-simple

test-lru: test.o
	$(CC) $(CFLAGS) $(INCLUDES) -o test-lru test.o $(LIBS) -L../ -lhemem-lru

gups.o: gups.c zipf.c  gups.h
	$(CC) $(CFLAGS) $(INCLUDES) -c gups.c zipf.c

test.o: test.c
	$(CC) $(CFLAGS) $(INCLUDES) -c test.c

clean:
	$(RM) *.o gups gups-simple gups-lru gups-lru-swap test test-simple test-lru
