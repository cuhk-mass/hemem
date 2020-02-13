#CFLAGS = -g -Wall -pthread
CFLAGS = -O2 -Wall -pthread
# CFLAGS = -g -Wall -pthread -DLOG_DEBUG
LDFLAGS = -pthread

all: mmgr_simple mmgr_linux mmgr_simple_mmm

mmgr_simple.o: mmgr_simple.c shared.h
mmgr_linux.o: mmgr_linux.c shared.h
memsim.o: memsim.c shared.h

memsim_mmm.o: CFLAGS += -DMMM
memsim_mmm.o: memsim_mmm.c shared.h

mmgr_simple_mmm.o: CFLAGS += -DMMM
mmgr_simple_mmm.o: mmgr_simple_mmm.c shared.h

mmgr_simple_mmm: mmgr_simple_mmm.o memsim_mmm.o
mmgr_simple: mmgr_simple.o memsim.o
mmgr_linux: mmgr_linux.o memsim.o

bench: all
	/bin/echo -ne "Benchmark\ttime [us]\tDRAM\tNVM\tTLB misses\tTLB hits\tpagefaults\n" > results.txt
	./mmgr_simple >> results.txt
	./mmgr_linux >> results.txt
	./mmgr_simple_mmm >> results.txt
	expand -t20 results.txt

clean:
	rm -f *.o mmgr_simple mmgr_simple_mmm mmgr_linux
