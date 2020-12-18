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

uint64_t hemem_pages_cnt = 0;
uint64_t other_pages_cnt = 0;
uint64_t total_pages_cnt = 0;
uint64_t throttle_cnt = 0;
uint64_t unthrottle_cnt = 0;

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
              __u64 pfn = ps->addr & GIGA_PFN_MASK;
            
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
                hemem_pages_cnt++;
              }
              else {
                other_pages_cnt++;
                struct hemem_page *hp = NULL;
                pthread_mutex_lock(&hash_lock);
                HASH_FIND(phh, pbuckets, &pfn, sizeof(__u64), hp);
                pthread_mutex_unlock(&hash_lock);

                if (hp == NULL) { 
                  // make hemem page for this sample
                  struct hemem_page *other_page = calloc(1, sizeof(struct hemem_page));
                  other_page->va = pfn;
                  other_page->accesses[j] = 1;
                  pthread_mutex_lock(&hash_lock);
                  HASH_ADD(phh, pbuckets, va, sizeof(__u64), other_page);
                  fprintf(stderr, "pebs addr %016llx\n", ps->addr);
                  pthread_mutex_unlock(&hash_lock);
                }
                else {
                  //fprintf(stderr, "pebs addr %016llx\n", ps->addr);
                  hp->accesses[j]++;
                }
              }
            
              total_pages_cnt++;
            }
	      }
  	      break;
        case PERF_RECORD_THROTTLE:
        case PERF_RECORD_UNTHROTTLE:
          fprintf(stderr, "%s event!\n",
              ph->type == PERF_RECORD_THROTTLE ? "THROTTLE" : "UNTHROTTLE");
          if (ph->type == PERF_RECORD_THROTTLE) {
              throttle_cnt++;
          }
          else {
              unthrottle_cnt++;
          }
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

static int hash_sort(struct hemem_page *a, struct hemem_page *b)
{
    return (a->va - b->va);
}

uint32_t prints_called = 0;

void pebs_print(void)
{

  HASH_SRT(phh, pbuckets, hash_sort);

  struct hemem_page *p, *tmp;
  pthread_mutex_lock(&hash_lock);
  HASH_ITER(phh, pbuckets, p, tmp) {
    fprintf(pebs_file, "0x%lx: %lu\t%lu\n", p->va, p->accesses[READ], p->accesses[WRITE]);
  }
  pthread_mutex_unlock(&hash_lock);

  /*
  FILE* pebsf;
  char filename[10];
  snprintf(filename, 10, "pebs%d.txt", prints_called);
  pebsf = fopen(filename, "w");
  assert(pebsf != NULL);
  prints_called++;

  fprintf(pebsf, "Hash Size: %u\n", HASH_CNT(phh, pbuckets));

  struct hemem_page *p, *tmp;
  FILE* mapsf = fopen("/proc/self/maps", "r");
  if (mapsf == NULL) {
    perror("fopen");
  }
  assert(mapsf != NULL);

  char *line = NULL;
  ssize_t nread;
  size_t len;
  __u64 start, end;
  int n;

  HASH_SRT(phh, pbuckets, hash_sort);

  nread = getline(&line, &len, mapsf);
  while (nread != -1) {
    fprintf(pebsf, "%s", line);

    n = sscanf(line, "%llX-%llX", &start, &end);
    if (n != 2) {
      fprintf(stderr, "error, invalid line: %s", line);
      assert(0);
    }
    
    pthread_mutex_lock(&hash_lock);
    HASH_ITER(phh, pbuckets, p, tmp) {
      if (p->va >= start && p->va < end) {
        //if (get_hemem_page(p->va) == NULL) {
          fprintf(pebsf, "\tvaddr 0x%lx: %zu %zu\n", p->va, p->accesses[READ], p->accesses[WRITE]);
        //}
      } 
    }
    pthread_mutex_unlock(&hash_lock);

    nread = getline(&line, &len, mapsf);
  }

  fclose(mapsf);
  fclose(pebsf);
  */
}

void pebs_clear(void)
{
  pthread_mutex_lock(&hash_lock);
  HASH_CLEAR(phh, pbuckets);
  
  struct hemem_page *dummy_page = calloc(1, sizeof(struct hemem_page));
  HASH_ADD(phh, pbuckets, va, sizeof(__u64), dummy_page);

  pthread_mutex_unlock(&hash_lock);
}

static void *pebs_stats(void *arg)
{
  internal_call = true;
   
  for (;;) {
    sleep(1);
    /*
    pthread_mutex_lock(&hash_lock);
    fprintf(stderr, "%u pebs pages touched\n", HASH_CNT(phh, pbuckets));
    HASH_CLEAR(phh, pbuckets);
  
    struct hemem_page *dummy_page = calloc(1, sizeof(struct hemem_page));
    HASH_ADD(phh, pbuckets, va, sizeof(__u64), dummy_page);

    pthread_mutex_unlock(&hash_lock); 
    */
    fprintf(stderr, "%lu hemem pages, %lu other pages, %lu total pages %lu throttle %lu unthrottle\n",
            hemem_pages_cnt, other_pages_cnt, total_pages_cnt, throttle_cnt, unthrottle_cnt);
    hemem_pages_cnt = other_pages_cnt = total_pages_cnt = throttle_cnt = unthrottle_cnt = 0;
  }

  internal_call = false;    
  return NULL;
}

void pebs_init(void)
{
  for (int i = 0; i < PEBS_NPROCS; i++) {
    //perf_page[i][READ] = perf_setup(0x1cd, 0x4, i);
    //perf_page[i][READ] = perf_setup(0x81d0, 0, i);
    perf_page[i][READ] = perf_setup(0x80d1, 0, i);
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

  pthread_t stats_thread;
  r = pthread_create(&stats_thread, NULL, pebs_stats, NULL);
  assert(r ==  0);
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
