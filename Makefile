CC = gcc
CFLAGS = -g -Wall -O2 -fPIC
#CFLAGS = -g3 -Wall -O0 -fPIC
#CFLAGS += -DCOALESCE
LDFLAGS = -shared
INCLUDES = -I/home/tstamler/linux/usr/include
LIBS = -lm -lpthread
HEMEM_LIBS = $(LIBS) -ldl -lsyscall_intercept -L/home/tstamler/Hoard/src -lhoard

default: all

all: hemem-libs

hemem-libs: libhemem-lru.so libhemem-simple.so libhemem-lru-swap.so libhemem.so

libhemem.so: hemem.o hemem-mmgr.o timer.o paging.o interpose.o coalesce.o aligned.o pebs.o
	$(CC) $(LDFLAGS) -o libhemem.so hemem.o timer.o paging.o hemem-mmgr.o interpose.o coalesce.o aligned.o $(HEMEM_LIBS)

libhemem-lru.so: hemem-lru.o lru.o timer.o paging.o interpose.o coalesce.o aligned.o pebs.o
	$(CC) $(LDFLAGS) -o libhemem-lru.so hemem-lru.o timer.o paging.o lru.o interpose.o coalesce.o aligned.o $(HEMEM_LIBS)

libhemem-simple.so: hemem-simple.o simple.o timer.o paging.o interpose.o coalesce.o aligned.o pebs.o
	$(CC) $(LDFLAGS) -o libhemem-simple.so hemem-simple.o timer.o paging.o simple.o interpose.o coalesce.o aligned.o $(HEMEM_LIBS)

libhemem-lru-swap.so: hemem-lru-swap.o lru_swap.o timer.o paging.o interpose.o coalesce.o aligned.o pebs.o
	$(CC) $(LDFLAGS) -o libhemem-lru-swap.so hemem-lru-swap.o timer.o paging.o lru_swap.o interpose.o coalesce.o aligned.o $(HEMEM_LIBS)

#libhemem.so: hemem.o hemem-mmgr.o timer.o interpose.o paging.o pebs.o
#	$(CC) $(LDFLAGS) -o libhemem.so hemem.o timer.o hemem-mmgr.o interpose.o paging.o pebs.o $(HEMEM_LIBS)

#libhemem-lru.so: hemem-lru.o lru.o timer.o interpose.o paging.o pebs.o
#	$(CC) $(LDFLAGS) -o libhemem-lru.so hemem-lru.o timer.o lru.o interpose.o paging.o pebs.o $(HEMEM_LIBS)

#libhemem-simple.so: hemem-simple.o simple.o timer.o interpose.o paging.o pebs.o
#	$(CC) $(LDFLAGS) -o libhemem-simple.so hemem-simple.o timer.o simple.o interpose.o paging.o pebs.o $(HEMEM_LIBS)

#libhemem-lru-swap.so: hemem-lru-swap.o lru_swap.o timer.o interpose.o paging.o pebs.o
#	$(CC) $(LDFLAGS) -o libhemem-lru-swap.so hemem-lru-swap.o timer.o lru_swap.o interpose.o paging.o pebs.o $(HEMEM_LIBS)

hemem.o: hemem.c hemem.h hemem-mmgr.h interpose.h paging.h pebs.h
	$(CC) $(CFLAGS) $(INCLUDES) -D ALLOC_HEMEM -c hemem.c -o hemem.o

hemem-lru.o: hemem.c hemem.h lru.h interpose.h paging.h
	$(CC) $(CFLAGS) $(INCLUDES) -D ALLOC_LRU -c hemem.c -o hemem-lru.o

hemem-simple.o: hemem.c hemem.h simple.h interpose.h paging.h
	$(CC) $(CFLAGS) $(INCLUDES) -D ALLOC_SIMPLE -c hemem.c -o hemem-simple.o

hemem-lru-swap.o: hemem.c hemem.h lru.h interpose.h paging.h
	$(CC) $(CFLAGS) $(INCLUDES) -D ALLOC_LRU -D LRU_SWAP -c hemem.c -o hemem-lru-swap.o

interpose.o: interpose.c interpose.h hemem.h
	$(CC) $(CFLAGS) $(INCLUDES) -c interpose.c

timer.o: timer.c timer.h
	$(CC) $(CFLAGS) $(INCLUDES) -c timer.c

hemem-mmgr.o: hemem-mmgr.c hemem-mmgr.h hemem.h
	$(CC) $(CFLAGS) $(INCLUDES) -c hemem-mmgr.c

lru.o: lru.c lru.h hemem.h
	$(CC) $(CFLAGS) $(INCLUDES) -c lru.c

simple.o: simple.c simple.h hemem.h
	$(CC) $(CFLAGS) $(INCLUDES) -c simple.c

coalesce.o: coalesce.c hash.h coalesce.h
	$(CC) $(CFLAGS) $(INCLUDES) -c coalesce.c

aligned.o: aligned.c hash.h aligned.h
	$(CC) $(CFLAGS) $(INCLUDES) -c aligned.c

lru_swap.o: lru.c lru.h hemem.h
	$(CC) $(CFLAGS) $(INCLUDES) -D LRU_SWAP -c lru.c -o lru_swap.o

paging.o: paging.c paging.h
	$(CC) $(CFLAGS) $(INCLUDES) -c paging.c

pebs.o: pebs.c pebs.h hemem.h
	$(CC) $(CFLAGS) $(INCLUDES) -c pebs.c

clean:
	$(RM) *.o *.so
