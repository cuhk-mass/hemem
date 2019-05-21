gups: gups.c zipf.c
	gcc -g -o gups gups.c zipf.c -lm -pthread -lvmem -lpmem
