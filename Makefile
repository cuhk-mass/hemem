CC = gcc
CFLAGS = -g -Wall -O2 -fPIC
#CFLAGS = -g3 -Wall -fPIC
LDFLAGS = -shared
INCLUDES = -I/usr/local/include
LIBS = -lm -lpthread -ldl

default: all

all: gups-simple gups-lru gups-lru-modified

gups-nohemem: gups-nohemem.o 
	$(CC) $(CFLAGS) $(INCLUDES) -o gups-nohemem gups-nohemem.o zipf.o timer.o $(LIBS) -L. -lpmem

gups-lru: gups.o libhemem-lru.so
	$(CC) $(CFLAGS) $(INCLUDES) -o gups-lru gups.o zipf.o $(LIBS) -L. -lhemem-lru

gups-simple: gups.o libhemem-simple.so
	$(CC) $(CFLAGS) $(INCLUDES) -o gups-simple gups.o zipf.o $(LIBS) -L. -lhemem-simple

gups-lru-modified: gups.o libhemem-modified-lru.so
	$(CC) $(CFLAGS) $(INCLUDES) -o gups-lru-modified gups.o zipf.o $(LIBS) -L. -lhemem-modified-lru

gups.o: gups.c zipf.c hemem.h timer.h gups.h
	$(CC) $(CFLAGS) $(INCLUDES) -c gups.c zipf.c

gups-nohemem.o: gups-nohemem.c zipf.c timer.h gups.h timer.c
	$(CC) $(CFLAGS) $(INCLUDES) -c gups-nohemem.c zipf.c timer.c

libhemem-lru.so: hemem-lru.o lru.o timer.o paging.o interpose.o
	$(CC) $(LDFLAGS) -o libhemem-lru.so hemem-lru.o timer.o paging.o lru.o interpose.o

libhemem-simple.so: hemem-simple.o simple.o timer.o paging.o interpose.o
	$(CC) $(LDFLAGS) -o libhemem-simple.so hemem-simple.o timer.o paging.o simple.o interpose.o

libhemem-modified-lru.so: hemem-modified-lru.o lru_modified.o timer.o paging.o interpose.o
	$(CC) $(LDFLAGS) -o libhemem-modified-lru.so hemem-modified-lru.o timer.o paging.o lru_modified.o interpose.o

hemem-lru.o: hemem.c hemem.h paging.h lru.h interpose.h
	$(CC) $(CFLAGS) $(INCLUDES) -D ALLOC_LRU -c hemem.c -o hemem-lru.o

hemem-simple.o: hemem.c hemem.h paging.h simple.h interpose.h
	$(CC) $(CFLAGS) $(INCLUDES) -D ALLOC_SIMPLE -c hemem.c -o hemem-simple.o

hemem-modified-lru.o: hemem.c hemem.h paging.h lru_modified.h interpose.h
	$(CC) $(CFLAGS) $(INCLUDES) -D ALLOC_LRU_MODIFIED -c hemem.c -o hemem-modified-lru.o

interpose.o: interpose.c interpose.h
	$(CC) $(CFLAGS) $(INCLUDES) -c interpose.c

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

clean:
	$(RM) *.o *.so gups-lru gups-simple gups-lru-modified
