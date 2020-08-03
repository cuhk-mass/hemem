/*
 * =====================================================================================
 *
 *       Filename:  pebs.c
 *
 *    Description:  
 *
 *        Version:  1.0
 *        Created:  07/24/20 17:41:35
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  YOUR NAME (), 
 *   Organization:  
 *
 * =====================================================================================
 */

#ifndef PEBS_H
#define PEBS_H

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

#include "hemem.h"
#include "uthash.h"


#define PERF_PAGES	(1 + (1 << 8))	// Has to be == 1+2^n, here 1MB
#define SAMPLE_PERIOD	1000000
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


void pebs_init(void);
void pebs_print(void);
void pebs_clear(void);

#endif
