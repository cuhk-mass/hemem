CFLAGS = -g -Wall -pthread
# CFLAGS = -g -Wall -pthread -DLOG_DEBUG
LDFLAGS = -pthread

all: mmgr_simple mmgr_linux

mmgr_simple.o: mmgr_simple.c shared.h
mmgr_linux.o: mmgr_linux.c shared.h
memsim.o: memsim.c shared.h

mmgr_simple: mmgr_simple.o memsim.o
mmgr_linux: mmgr_linux.o memsim.o

bench: all
	/bin/echo -ne "Benchmark\ttime [us]\tDRAM\tNVM\tTLB misses\tTLB hits\tpagefaults\n" > results.txt
	./mmgr_simple >> results.txt
	./mmgr_linux >> results.txt

clean:
	rm -f *.o mmgr_simple mmgr_linux
