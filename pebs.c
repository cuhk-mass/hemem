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
#include "pebs.h"

#ifdef USE_PEBS

#define PEBS_FILE   "pebs.txt"

static struct perf_event_mmap_page *perf_page[PEBS_NPROCS][NPBUFTYPES];

static struct hemem_page *pbuckets = NULL;
pthread_mutex_t hash_lock;

FILE *pebs_file;

static long
perf_event_open(struct perf_event_attr *hw_event, pid_t pid, 
    int cpu, int group_fd, unsigned long flags)
{
  int ret;

  ret = syscall(__NR_perf_event_open, hw_event, pid, cpu,
		group_fd, flags);
  return ret;
}


static struct perf_event_mmap_page* perf_setup(__u64 config, __u64 config1, __u64 cpu)
{
  struct perf_event_attr attr;

  memset(&attr, 0, sizeof(struct perf_event_attr));

  attr.type = PERF_TYPE_RAW;
  attr.size = sizeof(struct perf_event_attr);

  attr.config = config;
  attr.config1 = config1;
  attr.sample_period = SAMPLE_PERIOD;

  attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_WEIGHT | PERF_SAMPLE_ADDR;
  attr.disabled = 0;
  //attr.inherit = 1;
  attr.exclude_kernel = 1;
  attr.exclude_hv = 1;
  attr.exclude_callchain_kernel = 1;
  attr.exclude_callchain_user = 1;
  attr.precise_ip = 1;

  int pfd = perf_event_open(&attr, -1, cpu, -1, 0);
  if(pfd == -1) {
    perror("perf_event_open");
  }
  assert(pfd != -1);

  size_t mmap_size = sysconf(_SC_PAGESIZE) * PERF_PAGES;
  /* printf("mmap_size = %zu\n", mmap_size); */
  struct perf_event_mmap_page *p = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, pfd, 0);
  if(p == MAP_FAILED) {
    perror("mmap");
  }
  assert(p != MAP_FAILED);

  return p;
}

static void *hemem_measure(void *arg)
{
  internal_call = true;
  for(;;) {
    for (int i = 0; i < PEBS_NPROCS; i++) {
      for(int j = 0; j < NPBUFTYPES; j++) {
        struct perf_event_mmap_page *p = perf_page[i][j];
        char *pbuf = (char *)p + p->data_offset;

        __sync_synchronize();

        if(p->data_head == p->data_tail) {
          continue;
        }

        struct perf_event_header *ph = (void *)(pbuf + (p->data_tail % p->data_size));

        switch(ph->type) {
        case PERF_RECORD_SAMPLE:
          {
            struct perf_sample *ps = (void *)ph;
            if(ps->addr != 0) {
              __u64 pfn = ps->addr & HUGE_PFN_MASK;
            
              struct hemem_page* page = get_hemem_page(pfn);
              if (page != NULL) {
                pthread_mutex_lock(&(page->page_lock));
                struct hemem_page *hp = NULL;
                pthread_mutex_lock(&hash_lock);
                HASH_FIND(phh, pbuckets, &pfn, sizeof(__u64), hp);
                pthread_mutex_unlock(&hash_lock);
            
                if(hp == NULL) {
                  page->accesses[j] = 1;
                  pthread_mutex_lock(&hash_lock);
                  HASH_ADD(phh, pbuckets, va, sizeof(__u64), page);
                  pthread_mutex_unlock(&hash_lock);
                } else {
                  //assert(hp == page);
                  hp->accesses[j]++;
                }

                pthread_mutex_unlock(&(page->page_lock));
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
          //assert(!"NYI");
          break;
        }

        p->data_tail += ph->size;
      }
    }
  }
  internal_call = false;
}

void pebs_print(void)
{
  struct hemem_page *p, *tmp;
  pthread_mutex_lock(&hash_lock);
  HASH_ITER(phh, pbuckets, p, tmp) {
    fprintf(pebs_file, "0x%lx: %lu\t%lu\n", p->va, p->accesses[READ], p->accesses[WRITE]);
  }
  pthread_mutex_unlock(&hash_lock);
}

void pebs_clear(void)
{
  pthread_mutex_lock(&hash_lock);
  HASH_CLEAR(phh, pbuckets);
  
  struct hemem_page *dummy_page = calloc(1, sizeof(struct hemem_page));
  HASH_ADD(phh, pbuckets, va, sizeof(__u64), dummy_page);

  pthread_mutex_unlock(&hash_lock);
}

void pebs_init(void)
{
  for (int i = 0; i < PEBS_NPROCS; i++) {
    perf_page[i][READ] = perf_setup(0x1cd, 0x4, i);
    perf_page[i][WRITE] = perf_setup(0x82d0, 0, i);
  }

  pthread_mutex_init(&hash_lock, NULL);

  pebs_file = fopen(PEBS_FILE, "w");
  assert(pebs_file != NULL);

  struct hemem_page *dummy_page = calloc(1, sizeof(struct hemem_page));
  HASH_ADD(phh, pbuckets, va, sizeof(__u64), dummy_page);

  pthread_t thread;
  int r = pthread_create(&thread, NULL, hemem_measure, NULL);
  assert(r == 0);
}

#else

void pebs_print(void)
{
}

void pebs_clear(void)
{
}

void pebs_init(void)
{
}
#endif // USE_PEBS
