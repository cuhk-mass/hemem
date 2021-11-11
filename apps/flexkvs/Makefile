FLEXKVS_COMMON=hashtable.o ialloc.o jenkins_hash.o settings.o
BENCH_COMMON=rng.o commandline.o workload.o

TAS_CODE?=/home/tstamler/tas
HEMEM_CODE=/home/amanda/hemem/

CFLAGS = -std=gnu99 -g -Wall -Icommon/include -O3 -march=native \
	-I$(TAS_CODE)/include -I$(TAS_CODE)/lib/tas/include 
#CFLAGS += -DDEL_TEST
LDFLAGS = -pthread -g -O3
LDLIBS = -lrt -lm -lpthread -lrt -ldl

include ../common/Makefile.mtcp


all: flexkvs kvsbench
all-sockets: flexkvs kvsbench
all-mtcp: flexkvs-mtcp kvsbench-mtcp
all-ll: flexkvs-ll

flexkvs: $(addprefix server/,$(FLEXKVS_COMMON) main.o)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) -L$(HEMEM_CODE) -lhemem

kvsbench: $(addprefix bench/,$(BENCH_COMMON) main.o) \
		$(TAS_CODE)/lib/utils/timeout.o $(TAS_CODE)/lib/utils/utils.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

flexkvs-ll: $(addprefix server/,$(FLEXKVS_COMMON) main-ll.o) \
		$(TAS_CODE)/lib/libtas.so
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

flexkvs-mtcp: $(addprefix server/,$(FLEXKVS_COMMON:.o=.mtcp.o) main.mtcp.o)
	$(CC) $(LDFLAGS) $(MTCP_LDFLAGS) -o $@ $^ $(LDLIBS) $(MTCP_LDLIBS)

kvsbench-mtcp: $(addprefix bench/,$(BENCH_COMMON:.o=.mtcp.o) main.mtcp.o) \
		../utils/utils.o
	$(CC) $(LDFLAGS) $(MTCP_LDFLAGS) -o $@ $^ $(LDLIBS) $(MTCP_LDLIBS)

%.mtcp.o: %.c
	$(CC) $(CFLAGS) $(MTCP_CFLAGS) -c -o $@ $<

clean:
	rm -f */*.o flexkvs kvsbench flexkvs-mtcp kvsbench-mtcp flexkvs-ll

.PHONY: all all-sockets all-mtcp all-ll clean
