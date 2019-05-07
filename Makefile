gups: gups.c zipf.c
	gcc -o gups gups.c zipf.c -lm -pthread -lvmem -lpmem
