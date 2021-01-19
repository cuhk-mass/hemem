#ifndef HEMEM_PEBS_H
#define HEMEM_PEBS_H

#include <pthread.h>
#include <stdint.h>
#include <inttypes.h>
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>

#include "hemem.h"

#define PEBS_KSWAPD_INTERVAL   (1000000) // in us (1s)
#define PEBS_KSWAPD_MIGRATE_RATE  (50UL * 1024UL * 1024UL * 1024UL) // 50GB
#define HOT_READ_THRESHOLD     (3)
#define HOT_WRITE_THRESHOLD    (3)
#define MIGRATION_STOP_THRESHOLD (10)

#define PEBS_NPROCS 64
#define PERF_PAGES	(1 + (1 << 8))	// Has to be == 1+2^n, here 1MB
#define SAMPLE_PERIOD	10007
//#define SAMPLE_PERIOD 5003
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
void pebs_lock();
void pebs_unlock();


#endif /*  HEMEM_LRU_MODIFIED_H  */
