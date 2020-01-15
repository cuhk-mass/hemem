CC = gcc
CFLAGS = -g -Wall
INCLUDES = -I/root/hmem/linux/usr/include
LIBS = -lm -lpthread

default: gups

all: gups tester

gups: gups.o hemem.o timer.o paging.o
	$(CC) $(CFLAGS) $(INCLUDES) -o gups gups.o zipf.o hemem.o timer.o paging.o $(LIBS)

gups.o: gups.c zipf.c hemem.h timer.h
	$(CC) $(CFLAGS) $(INCLUDES) -c gups.c zipf.c

hemem.o: hemem.c hemem.h paging.h
	$(CC) $(CFLAGS) $(INCLUDES) -c hemem.c

timer.o: timer.c timer.h
	$(CC) $(CFLAGS) $(INCLUDES) -c timer.c

paging.o: paging.c paging.h
	$(CC) $(CFLAGS) $(INCLUDES) -c paging.c

tester: test.c
	$(CC) -o tester test.c $(LIBS)

clean:
	$(RM) *.o *~ gups tester

