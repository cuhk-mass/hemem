#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <pthread.h>
#include <asm/unistd.h>
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>
#include <sys/mman.h>

#include "memsim/uthash.h"

// Handy size macros
#define KB(x)		(((uint64_t)x) * 1024)
#define MB(x)		(KB(x) * 1024)
#define GB(x)		(MB(x) * 1024)
#define TB(x)		(GB(x) * 1024)

#define PERF_PAGES	(1 + (1 << 8))	// Has to be == 1+2^n, here 1MB
#define SAMPLE_PERIOD	100000
//#define SAMPLE_FREQ	100

struct perf_sample {
  struct perf_event_header header;
  __u64	ip;
  __u32 pid, tid;    /* if PERF_SAMPLE_TID */
  __u64 addr;        /* if PERF_SAMPLE_ADDR */
  __u64 weight;      /* if PERF_SAMPLE_WEIGHT */
  /* __u64 data_src;    /\* if PERF_SAMPLE_DATA_SRC *\/ */
};

enum pbuftype {
  READ = 0,
  WRITE = 1,
  NPBUFTYPES
};

static struct perf_event_mmap_page *perf_page[NPBUFTYPES];

static long
perf_event_open(struct perf_event_attr *hw_event, pid_t pid,
		int cpu, int group_fd, unsigned long flags)
{
  int ret;

  ret = syscall(__NR_perf_event_open, hw_event, pid, cpu,
		group_fd, flags);
  return ret;
}

static struct perf_event_mmap_page *
perf_setup(__u64 config, __u64 config1)
{
  struct perf_event_attr attr;

  memset(&attr, 0, sizeof(struct perf_event_attr));
 
  /* attr.type = PERF_TYPE_HARDWARE; */
  /* attr.type = PERF_TYPE_HW_CACHE; */
  attr.type = PERF_TYPE_RAW;
  attr.size = sizeof(struct perf_event_attr);
  /* attr.config = PERF_COUNT_HW_INSTRUCTIONS; */
  /* attr.config = PERF_COUNT_HW_CACHE_REFERENCES; */
  /* attr.config = PERF_COUNT_HW_CACHE_LL | (PERF_COUNT_HW_CACHE_OP_READ << 8) | (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16); */
  
  /* attr.config = 0x81d0;	// mem_inst_retired.all_loads */

  // cpu/config=0x1cd,config1=0x4,name=mem_trans_retired.load_latency_gt_4/
  /* attr.config = 0x1cd; */
  /* attr.config1 = 0x4; */

  /* attr.config = 0x82d0;		// mem_inst_retired.all_stores */

  attr.config = config;
  attr.config1 = config1;
  attr.sample_period = SAMPLE_PERIOD;

  /* attr.sample_freq = SAMPLE_FREQ; */
  /* attr.freq = 1; */
  
  attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_WEIGHT | PERF_SAMPLE_ADDR;
  attr.disabled = 0;
  attr.exclude_kernel = 1;
  attr.exclude_hv = 1;
  attr.exclude_callchain_kernel = 1;
  attr.exclude_callchain_user = 1;
  attr.precise_ip = 1;

  int pfd = perf_event_open(&attr, 0, -1, -1, 0);
  if(pfd == -1) {
    perror("perf_event_open");
  }
  assert(pfd != -1);

  size_t mmap_size = sysconf(_SC_PAGESIZE) * PERF_PAGES;
  /* printf("mmap_size = %zu\n", mmap_size); */
  struct perf_event_mmap_page *p =
    mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, pfd, 0);
  if(p == MAP_FAILED) {
    perror("mmap");
  }
  assert(p != MAP_FAILED);
  
  return p;
}

#define BASE_PAGE_SIZE	KB(4)
#define HUGE_PAGE_SIZE	MB(2)
#define GIGA_PAGE_SIZE	GB(1)

// Page offset masks
#define BASE_PAGE_MASK	(BASE_PAGE_SIZE - 1)
#define HUGE_PAGE_MASK	(HUGE_PAGE_SIZE - 1)
#define GIGA_PAGE_MASK	(GIGA_PAGE_SIZE - 1)

// Page frame number masks
#define BASE_PFN_MASK	(BASE_PAGE_MASK ^ UINT64_MAX)
#define HUGE_PFN_MASK	(HUGE_PAGE_MASK ^ UINT64_MAX)
#define GIGA_PFN_MASK	(GIGA_PAGE_MASK ^ UINT64_MAX)

#define SLOWMEM_SIZE	GB(1)
#define WORKSET_SIZE	SLOWMEM_SIZE

struct perf_bucket {
  __u64			vaddr;
  size_t		accesses;
  UT_hash_handle	hh;
};

static struct perf_bucket *pbuckets = NULL;

static void *hemem_measure(void *arg)
{
  for(;;) {
    for(int i = 0; i < 2; i++) {
      struct perf_event_mmap_page *p = perf_page[i];
      char *pbuf = (char *)p + p->data_offset;
    
      __sync_synchronize();

      if(p->data_head == p->data_tail) {
	continue;
      }

      /* fprintf(stderr, "%d - head: %llu, tail: %llu\n", i, p->data_head, p->data_tail); */
    
      struct perf_event_header *ph =
	(void *)(pbuf + (p->data_tail % p->data_size));

      switch(ph->type) {
      case PERF_RECORD_SAMPLE:
	{
	  struct perf_sample *ps = (void *)ph;
	  /* fprintf(stderr, "%s, pid = %u, tid = %u, addr = 0x%llx, ip = 0x%llx, weight = %llu\n", */
	  /* 	  i == READ ? "READ" : "WRITE", ps->pid, ps->tid, ps->addr, ps->ip, ps->weight); */

	  if(ps->addr != 0) {
	    __u64 pfn = ps->addr & BASE_PFN_MASK;
	    
	    struct perf_bucket *pb = NULL;
	    HASH_FIND(hh, pbuckets, &pfn, sizeof(__u64), pb);
	    if(pb == NULL) {
	      pb = calloc(1, sizeof(struct perf_bucket));
	      assert(pb != NULL);
	      pb->vaddr = pfn;
	      pb->accesses = 1;
	      HASH_ADD(hh, pbuckets, vaddr, sizeof(__u64), pb);
	    } else {
	      pb->accesses++;
	    }
	  }
	}
	break;

      case PERF_RECORD_THROTTLE:
      case PERF_RECORD_UNTHROTTLE:
	fprintf(stderr, "%s event!\n",
		ph->type == PERF_RECORD_THROTTLE ? "THROTTLE" : "UNTHROTTLE");
	break;
	
      default:
	fprintf(stderr, "Unknown type %u\n", ph->type);
	assert(!"NYI");
	break;
      }
    
      p->data_tail += ph->size;
    }
  }
}

#define RAND_WITHIN(x)	(((double)rand() / RAND_MAX) * (x))

static volatile char *gups_buf;

static void gups(size_t iters, uint64_t hotset_start, uint64_t hotset_size,
		 double hotset_prob, uint64_t workset_size)
{
  assert(hotset_start + hotset_size <= workset_size);

  fprintf(stderr, "gups(iters = %zu, hotset_start = 0x%" PRIx64 ", hotset_size = 0x%" PRIx64 ", hotset_prob = %.2f, workset_size = 0x%" PRIx64 ")\n",
      iters, hotset_start, hotset_size, hotset_prob, workset_size);

  // GUPS with hotset
  for(size_t i = 0; i < iters; i++) {
    uint64_t a;

    if(RAND_WITHIN(1) < hotset_prob) {
      // Hot set
      a = hotset_start + (uint64_t)RAND_WITHIN(hotset_size);
    } else {
      // Entire working set
      a = (uint64_t)RAND_WITHIN(workset_size);
    }

    // Read&update
    char c = gups_buf[a];
    c++;
    gups_buf[a] = c;
  }
}

int main(int argc, char *argv[])
{
  if(argc < 2) {
    printf("Usage: %s HOTSET-SIZE\n", argv[0]);
    exit(EXIT_FAILURE);
  }

  uint64_t hotset_size = atoll(argv[1]);
  
  /* perf_page[READ] = perf_setup(0x81d0, 0); */
  perf_page[READ] = perf_setup(0x1cd, 0x4);
  perf_page[WRITE] = perf_setup(0x82d0, 0);

  pthread_t thread;
  int r = pthread_create(&thread, NULL, hemem_measure, NULL);
  assert(r == 0);

  gups_buf = mmap(NULL, WORKSET_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(gups_buf != MAP_FAILED);
  
  // GUPS!
  fprintf(stderr, "GUPS buffer at %p\n", gups_buf);
  gups(100000000, 0, hotset_size, 0.9, WORKSET_SIZE);

  struct perf_bucket *p, *tmp;
  HASH_ITER(hh, pbuckets, p, tmp) {
    printf("vaddr 0x%llx: %zu", p->vaddr, p->accesses);

    if(p->vaddr >= (uint64_t)gups_buf && p->vaddr < (uint64_t)gups_buf + WORKSET_SIZE) {
      printf(" GUPS offset 0x%llx\n", p->vaddr - (uint64_t)gups_buf);
    } else {
      printf("\n");
    }
  }
  
  return 0;
}
