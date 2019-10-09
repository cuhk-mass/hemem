CC = gcc
CFLAGS = -g -Wall
INCLUDES = -I/root/hmem/linux/usr/include
LIBS = -lm -lpthread

default: gups

all: gups tester

gups: gups.o hemem.o timer.o
	$(CC) $(CFLAGS) $(INCLUDES) -o gups gups.o zipf.o hmem.o timer.o $(LIBS)

gups.o: gups.c zipf.c hemem.h timer.h
	$(CC) $(CFLAGS) -c gups.c zipf.c

hemem.o: hemem.h hemem.c
	$(CC) $(CFLAGS) $(INCLUDES) -c hemem.c

timer.o: timer.h timer.c
	$(CC) $(CFLAGS) $(INCLUDES) -c timer.c

tester: test.c
	$(CC) -o tester test.c $(LIBS)

clean:
	$(RM) *.o *~ gups tester

