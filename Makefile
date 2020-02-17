CC = gcc
CFLAGS = -g -Wall -O3
INCLUDES = -I/root/hmem/linux/usr/include
LIBS = -lm -lpthread

default: all

all: gups-simple gups-lru gups-modified-lru tester

gups-lru: gups.o hemem-lru.o timer.o paging.o lru.o
	$(CC) $(CFLAGS) $(INCLUDES) -o gups-lru gups.o zipf.o hemem-lru.o timer.o paging.o lru.o $(LIBS)

gups-simple: gups.o hemem-simple.o timer.o paging.o simple.o
	$(CC) $(CFLAGS) $(INCLUDES) -o gups-simple gups.o zipf.o hemem-simple.o timer.o paging.o simple.o $(LIBS)

gups-modified-lru: gups.o hemem-modified-lru.o timer.o paging.o lru_modified.o
	$(CC) $(CFLAGS) $(INCLUDES) -o gups-lru-modified gups.o zipf.o hemem-modified-lru.o timer.o paging.o lru_modified.o $(LIBS)

gups.o: gups.c zipf.c hemem.h timer.h gups.h
	$(CC) $(CFLAGS) $(INCLUDES) -c gups.c zipf.c

hemem-lru.o: hemem.c hemem.h paging.h lru.h
	$(CC) $(CFLAGS) $(INCLUDES) -D ALLOC_LRU -c hemem.c -o hemem-lru.o

hemem-simple.o: hemem.c hemem.h paging.h simple.h
	$(CC) $(CFLAGS) $(INCLUDES) -D ALLOC_SIMPLE -c hemem.c -o hemem-simple.o

hemem-modified-lru.o: hemem.c hemem.h paging.h lru_modified.h
	$(CC) $(CFLAGS) $(INCLUDES) -D ALLOC_LRU_MODIFIED -c hemem.c -o hemem-modified-lru.o

timer.o: timer.c timer.h
	$(CC) $(CFLAGS) $(INCLUDES) -c timer.c

paging.o: paging.c paging.h
	$(CC) $(CFLAGS) $(INCLUDES) -c paging.c

lru.o: lru.c lru.h hemem.h
	$(CC) $(CFLAGS) $(INCLUDES) -c lru.c

simple.o: simple.c simple.h hemem.h
	$(CC) $(CFLAGS) $(INCLUDES) -c simple.c

lru_modified.o: lru_modified.c lru_modified.h hemem.h
	$(CC) $(CFLAGS) $(INCLUDES) -c lru_modified.c

tester: test.c
	$(CC) -o tester test.c $(LIBS)

clean:
	$(RM) *.o gups-lru gups-simple gups-lru-modified tester memsim/mmgr_simple mmgr_linux

