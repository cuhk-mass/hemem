all: gups test

gups: gups.c zipf.c
	gcc -I/root/hmem/linux/usr/include -g -o gups gups.c zipf.c -lm -pthread -lvmem -lpmem

test: test.c
	gcc -o test test.c -pthread

pebs: CFLAGS += -g -O2 -pthread
#pebs: CFLAGS += -g -pthread
