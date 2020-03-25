CC = gcc
#CFLAGS = -g -Wall -O2 -fPIC
CFLAGS = -g3 -Wall -O0 -fPIC
LDFLAGS = -shared
INCLUDES = -I/root/hmem/linux/usr/include
LIBS = -lm -lpthread -ldl

default: all

all: gups-simple gups-lru gups-lru-swap test test-simple

test-simple: test.o libhemem-simple.so
	$(CC) $(CFLAGS) $(INCLUDES) -o test-simple test.o $(LIBS) -L. -lhemem-simple

test: test.o libhemem-lru.so
	$(CC) $(CFLAGS) $(INCLUDES) -o test test.o $(LIBS) -L. -lhemem-lru

gups-lru: gups.o libhemem-lru.so
	$(CC) $(CFLAGS) $(INCLUDES) -o gups-lru gups.o zipf.o $(LIBS) -L. -lhemem-lru

gups-simple: gups.o libhemem-simple.so
	$(CC) $(CFLAGS) $(INCLUDES) -o gups-simple gups.o zipf.o $(LIBS) -L. -lhemem-simple

gups-lru-swap: gups.o libhemem-lru-swap.so
	$(CC) $(CFLAGS) $(INCLUDES) -o gups-lru-swap gups.o zipf.o $(LIBS) -L. -lhemem-lru-swap

gups.o: gups.c zipf.c hemem.h timer.h gups.h
	$(CC) $(CFLAGS) $(INCLUDES) -c gups.c zipf.c

libhemem-lru.so: hemem-lru.o lru.o timer.o paging.o interpose.o
	$(CC) $(LDFLAGS) -o libhemem-lru.so hemem-lru.o timer.o paging.o lru.o interpose.o

libhemem-simple.so: hemem-simple.o simple.o timer.o paging.o interpose.o
	$(CC) $(LDFLAGS) -o libhemem-simple.so hemem-simple.o timer.o paging.o simple.o interpose.o

libhemem-lru-swap.so: hemem-lru-swap.o lru_swap.o timer.o paging.o interpose.o
	$(CC) $(LDFLAGS) -o libhemem-lru-swap.so hemem-lru-swap.o timer.o paging.o lru_swap.o interpose.o

hemem-lru.o: hemem.c hemem.h paging.h lru.h interpose.h
	$(CC) $(CFLAGS) $(INCLUDES) -D ALLOC_LRU -c hemem.c -o hemem-lru.o

hemem-simple.o: hemem.c hemem.h paging.h simple.h interpose.h
	$(CC) $(CFLAGS) $(INCLUDES) -D ALLOC_SIMPLE -c hemem.c -o hemem-simple.o

hemem-lru-swap.o: hemem.c hemem.h paging.h lru.h interpose.h
	$(CC) $(CFLAGS) $(INCLUDES) -D ALLOC_LRU -D LRU_SWAP -c hemem.c -o hemem-lru-swap.o

interpose.o: interpose.c interpose.h hemem.h
	$(CC) $(CFLAGS) $(INCLUDES) -c interpose.c

timer.o: timer.c timer.h
	$(CC) $(CFLAGS) $(INCLUDES) -c timer.c

paging.o: paging.c paging.h
	$(CC) $(CFLAGS) $(INCLUDES) -c paging.c

lru.o: lru.c lru.h hemem.h
	$(CC) $(CFLAGS) $(INCLUDES) -c lru.c

simple.o: simple.c simple.h hemem.h
	$(CC) $(CFLAGS) $(INCLUDES) -c simple.c

lru_swap.o: lru.c lru.h hemem.h
	$(CC) $(CFLAGS) $(INCLUDES) -D LRU_SWAP -c lru.c -o lru_swap.o

test.o: test.c timer.h hemem.h
	$(CC) $(CFLAGS) $(INCLUDES) -c test.c

clean:
	$(RM) *.o *.so gups-lru gups-simple gups-lru-swap
