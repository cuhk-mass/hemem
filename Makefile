all: gups test

gups: gups.c zipf.c
	gcc -I/root/hmem/linux/usr/include -g -o gups gups.c zipf.c -lm -pthread -lpmem

test: test.c
	gcc -o test test.c -pthread
