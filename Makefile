gups: gups.c zipf.c
	gcc -I/root/hmem/linux/usr/include -g -o gups gups.c zipf.c -lm -pthread -lvmem -lpmem
