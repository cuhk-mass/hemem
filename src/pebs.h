#ifndef HEMEM_PEBS_H
#define HEMEM_PEBS_H

#include <pthread.h>
#include <stdint.h>
#include <inttypes.h>
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>

#include "hemem.h"

#define PEBS_KSWAPD_INTERVAL      (10000) // in us (10ms)
#define PEBS_KSWAPD_MIGRATE_RATE  (10UL * 1024UL * 1024UL * 1024UL) // 10GB
#define HOT_READ_THRESHOLD        (4)
#define HOT_WRITE_THRESHOLD       (4)
#define PEBS_COOLING_THRESHOLD    (10)

#define HOT_RING_REQS_THRESHOLD   (1024*1024)
#define COLD_RING_REQS_THRESHOLD  (128)
#define CAPACITY                  (16*1024*1024)
#define COOLING_PAGES             (8192)

#define PEBS_NPROCS 4
#define PERF_PAGES	(1 + (1 << 14))	// Has to be == 1+2^n
//#define SAMPLE_PERIOD	10007
#define SAMPLE_PERIOD 19997
//#define SAMPLE_FREQ	100


#define SCANNING_THREAD_CPU (FAULT_THREAD_CPU + 1)
#define MIGRATION_THREAD_CPU (SCANNING_THREAD_CPU + 1)

struct perf_sample {
  struct perf_event_header header;
  __u64	ip;
  __u32 pid, tid;    /* if PERF_SAMPLE_TID */
  __u64 addr;        /* if PERF_SAMPLE_ADDR */
  __u64 weight;      /* if PERF_SAMPLE_WEIGHT */
  /* __u64 data_src;    /\* if PERF_SAMPLE_DATA_SRC *\/ */
};

enum pbuftype {
  DRAMREAD = 0,
  NVMREAD = 1,  
  WRITE = 2,
  NPBUFTYPES
};

void *pebs_kswapd();
struct hemem_page* pebs_pagefault(void);
struct hemem_page* pebs_pagefault_unlocked(void);
void pebs_init(void);
void pebs_remove_page(struct hemem_page *page);
void pebs_stats();
void pebs_shutdown();

#endif /*  HEMEM_LRU_MODIFIED_H  */
