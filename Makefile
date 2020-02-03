CC = gcc
CFLAGS = -g -Wall
INCLUDES = -I/root/hmem/linux/usr/include
LIBS = -lm -lpthread

default: gups

all: gups tester

gups: gups.o hemem.o timer.o paging.o lru.o
	$(CC) $(CFLAGS) $(INCLUDES) -o gups gups.o zipf.o hemem.o timer.o paging.o lru.o $(LIBS)

gups.o: gups.c zipf.c hemem.h timer.h
	$(CC) $(CFLAGS) $(INCLUDES) -c gups.c zipf.c

hemem.o: hemem.c hemem.h paging.h lru.h
	$(CC) $(CFLAGS) $(INCLUDES) -c hemem.c

timer.o: timer.c timer.h
	$(CC) $(CFLAGS) $(INCLUDES) -c timer.c

paging.o: paging.c paging.h
	$(CC) $(CFLAGS) $(INCLUDES) -c paging.c

lru.o: lru.c lru.h
	$(CC) $(CFLAGS) $(INCLUDES) -c lru.c

tester: test.c
	$(CC) -o tester test.c $(LIBS)

clean:
	$(RM) *.o gups tester memsim/mmgr_simple mmgr_linux

