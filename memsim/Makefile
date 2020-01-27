CFLAGS = -g -Wall -pthread
LDFLAGS = -pthread

all: getmem_simple getmem_linux

getmem_simple.o: getmem_simple.c shared.h
getmem_linux.o: getmem_linux.c shared.h
memsim.o: memsim.c shared.h

getmem_simple: getmem_simple.o memsim.o
getmem_linux: getmem_linux.o memsim.o

bench: all
	/bin/echo -ne "Benchmark\ttime [us]\tDRAM\tNVM\tTLB misses\tTLB hits\tpagefaults\n" > results.txt
	./getmem_simple >> results.txt
	./getmem_linux >> results.txt

clean:
	rm -f *.o getmem_simple getmem_linux
