CC = gcc
CFLAGS = -g -Wall -O2 -fPIC
#CFLAGS = -g3 -Wall -O0 -fPIC
LDFLAGS = -shared
INCLUDES = -I/home/amanda/linux/usr/include
LIBS = -lm -lpthread
HEMEM_LIBS = $(LIBS) -ldl -lsyscall_intercept -L/home/amanda/Hoard/src -lhoard

default: all

all: hemem-libs

hemem-libs: libhemem-lru.so libhemem-simple.so libhemem-lru-swap.so libhemem.so

libhemem.so: hemem.o pebs.o timer.o interpose.o fifo.o
	$(CC) $(LDFLAGS) -o libhemem.so hemem.o timer.o interpose.o pebs.o fifo.o $(HEMEM_LIBS)

libhemem-lru.so: hemem-lru.o lru.o timer.o interpose.o paging.o fifo.o
	$(CC) $(LDFLAGS) -o libhemem-lru.so hemem-lru.o timer.o lru.o interpose.o paging.o fifo.o $(HEMEM_LIBS)

libhemem-simple.so: hemem-simple.o simple.o timer.o interpose.o paging.o fifo.o
	$(CC) $(LDFLAGS) -o libhemem-simple.so hemem-simple.o timer.o simple.o interpose.o paging.o fifo.o $(HEMEM_LIBS)

libhemem-lru-swap.so: hemem-lru-swap.o lru_swap.o timer.o interpose.o paging.o fifo.o
	$(CC) $(LDFLAGS) -o libhemem-lru-swap.so hemem-lru-swap.o timer.o lru_swap.o interpose.o paging.o fifo.o $(HEMEM_LIBS)

hemem.o: hemem.c hemem.h pebs.h interpose.h fifo.h
	$(CC) $(CFLAGS) $(INCLUDES) -D ALLOC_HEMEM -c hemem.c -o hemem.o

hemem-lru.o: hemem.c hemem.h lru.h interpose.h paging.h fifo.h
	$(CC) $(CFLAGS) $(INCLUDES) -D ALLOC_LRU -c hemem.c -o hemem-lru.o

hemem-simple.o: hemem.c hemem.h simple.h interpose.h paging.h fifo.h
	$(CC) $(CFLAGS) $(INCLUDES) -D ALLOC_SIMPLE -c hemem.c -o hemem-simple.o

hemem-lru-swap.o: hemem.c hemem.h lru.h interpose.h paging.h fifo.h
	$(CC) $(CFLAGS) $(INCLUDES) -D ALLOC_LRU -D LRU_SWAP -c hemem.c -o hemem-lru-swap.o

interpose.o: interpose.c interpose.h hemem.h
	$(CC) $(CFLAGS) $(INCLUDES) -c interpose.c

timer.o: timer.c timer.h
	$(CC) $(CFLAGS) $(INCLUDES) -c timer.c

hemem-mmgr.o: hemem-mmgr.c hemem-mmgr.h hemem.h	fifo.h
	$(CC) $(CFLAGS) $(INCLUDESV) -c hemem-mmgr.c

lru.o: lru.c lru.h hemem.h fifo.h
	$(CC) $(CFLAGS) $(INCLUDES) -c lru.c

simple.o: simple.c simple.h hemem.h
	$(CC) $(CFLAGS) $(INCLUDES) -c simple.c

lru_swap.o: lru.c lru.h hemem.h fifo.h
	$(CC) $(CFLAGS) $(INCLUDES) -D LRU_SWAP -c lru.c -o lru_swap.o

paging.o: paging.c paging.h
	$(CC) $(CFLAGS) $(INCLUDES) -c paging.c

pebs.o: pebs.c pebs.h hemem.h fifo.h
	$(CC) $(CFLAGS) $(INCLUDES) -c pebs.c

fifo.o: fifo.c fifo.h hemem.h
	$(CC) $(CFLAGS) $(INCLUDES) -c fifo.c

clean:
	$(RM) *.o *.so
