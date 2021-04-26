CC = gcc
CFLAGS = -g -Wall -O2 
#CFLAGS = -g3 -Wall -O0
INCLUDES = -I/home/amanda/linux/usr/include
LIBS = -lm -lpthread

default: all

all: gups-pebs gups-simple gups-lru gups-lru-swap test-simple test-lru test gups-writes gups-hotset-move #gups

gups-small: gups-small.o
	$(CC) $(CFLAGS) $(INCLUDES) -o gups-small gups-small.o ../timer.o $(LIBS)

gups: gups.o
	$(CC) $(CFLAGS) $(INCLUDES) -o gups gups.o zipf.o ../timer.o $(LIBS)

gups-writes: gups-writes.o
	$(CC) $(CFLAGS) $(INCLUDES) -o gups-writes gups-writes.o zipf.o $(LIBS) -L../ -lhemem

gups-hotset-move: gups-hotset-move.o
	$(CC) $(CFLAGS) $(INCLUDES) -o gups-hotset-move gups-hotset-move.o zipf.o $(LIBS) -L../ -lhemem

gups-pebs: gups.o
	$(CC) $(CFLAGS) $(INCLUDES) -o gups-pebs gups.o zipf.o $(LIBS) -L../ -lhemem

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

gups-small.o: gups-small.c 
	$(CC) $(CFLAGS) $(INCLUDES) -c gups-small.c

gups-writes.o: gups-writes.c zipf.c gups.h
	$(CC) $(CFLAGS) $(INCLUDES) -c gups-writes.c zipf.c

gups-hotset-move.o: gups-hotset-move.c zipf.c gups.h
	$(CC) $(CFLAGS) $(INCLUDES) -c gups-hotset-move.c zipf.c


clean:
	$(RM) *.o gups gups-simple gups-lru gups-lru-swap test test-simple test-lru
