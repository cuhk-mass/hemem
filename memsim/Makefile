CFLAGS = -g3 -Wall -pthread
#CFLAGS += -DLOG_DEBUG
CFLAGS += -O2
LDFLAGS = -pthread

# 1MB hotset
#HOTSET_SIZE = 1048576
#HOTSET_SIZE = 45097156608
#HOTSET_SIZE = 22548578304
HOTSET_SIZE = 137438953472

all: mmgr_simple mmgr_linux mmgr_simple_mmm mmgr_hemem pgsweep

pgsweep: pgsweep.o
pgsweep.o: pgsweep.c

mmgr_simple.o: mmgr_simple.c shared.h
mmgr_linux.o: mmgr_linux.c shared.h
mmgr_hemem.o: mmgr_hemem.c shared.h
memsim.o: memsim.c shared.h

memsim_mmm.o: CFLAGS += -DMMM
memsim_mmm.o: memsim_mmm.c shared.h

mmgr_simple_mmm.o: CFLAGS += -DMMM
mmgr_simple_mmm.o: mmgr_simple_mmm.c shared.h

mmgr_simple_mmm: mmgr_simple_mmm.o memsim_mmm.o
mmgr_simple: mmgr_simple.o memsim.o
mmgr_linux: mmgr_linux.o memsim.o
mmgr_hemem: mmgr_hemem.o memsim.o

%.results.txt: %
	./$< $(HOTSET_SIZE) > $@

# results.txt: mmgr_simple.results.txt mmgr_simple_mmm.results.txt mmgr_hemem.results.txt mmgr_linux.results.txt
results.txt: mmgr_simple_mmm.results.txt mmgr_hemem.results.txt
	/bin/echo -ne "Benchmark\ttime [ms]\tDRAM accesses\tNVM accesses\tTLB misses\tTLB hits\tTLB shootdowns\tpagefaults\t(MMM misses)\n" > results.txt
	cat $^ >> results.txt

.PHONY: bench

bench: results.txt
	expand -t20 results.txt

clean:
	rm -f *.o mmgr_simple mmgr_simple_mmm mmgr_linux mmgr_hemem pgsweep
