#include <stdlib.h>
#include <pthread.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <pthread.h>
#include <assert.h>
#include <sys/time.h>
#include <unistd.h>
#include <asm/unistd.h>
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>
#include <sys/mman.h>


#include "hemem.h"
#include "pebs.h"
#include "timer.h"


static struct fifo_list dram_hot_list;
static struct fifo_list dram_cold_list;
static struct fifo_list dram_locked_list;
static struct fifo_list nvm_hot_list;
static struct fifo_list nvm_cold_list;
static struct fifo_list nvm_locked_list;
static struct fifo_list dram_free_list;
static struct fifo_list nvm_free_list;
static pthread_mutex_t global_lock = PTHREAD_MUTEX_INITIALIZER;
uint64_t pebs_runs = 0;
static volatile bool in_kscand = false;

uint64_t hemem_pages_cnt = 0;
uint64_t other_pages_cnt = 0;
uint64_t total_pages_cnt = 0;
uint64_t throttle_cnt = 0;
uint64_t unthrottle_cnt = 0;
uint64_t locked_pages = 0;


static struct perf_event_mmap_page *perf_page[PEBS_NPROCS][NPBUFTYPES];


static long perf_event_open(struct perf_event_attr *hw_event, pid_t pid, 
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

// assumes page lock is held
void make_hot(struct hemem_page* page)
{
  assert(page != NULL);
  assert(page->va != 0);
  
  if (page->hot) {
    if (page->in_dram) {
      if (page->stop_migrating) {
        assert(page->list == &dram_locked_list);
      }
      else {
        assert(page->list == &dram_hot_list);
      }
    }
    else {
      if (page->stop_migrating) {
        assert(page->list == &nvm_locked_list);
      }
      else {
        assert(page->list == &nvm_hot_list);
      }
    }
    // page is already in hot list for memory type, nothing to do
    return;
  }

  if (page->in_dram) {
    assert(page->list == &dram_cold_list);
    page_list_remove_page(&dram_cold_list, page);
    if (page->stop_migrating) {
      enqueue_fifo(&dram_locked_list, page);
      return;
    }
    enqueue_fifo(&dram_hot_list, page);
    page->hot = true;
  }
  else {
    assert(page->list == &nvm_cold_list);
    page_list_remove_page(&nvm_cold_list, page);
    if (page->stop_migrating) {
      enqueue_fifo(&nvm_locked_list, page);
      return;
    }
    enqueue_fifo(&nvm_hot_list, page);
    page->hot = true;
  }
}

void make_cold()
{
  struct hemem_page *p;

  p = dequeue_fifo(&dram_hot_list);
  while (p != NULL) {
    pthread_mutex_lock(&p->page_lock);
    assert(p->in_dram);
    p->hot = false;
    p->accesses[DRAMREAD] = p->accesses[NVMREAD] = p->accesses[WRITE] = 0;

    if (p->stop_migrating) {
      enqueue_fifo(&dram_locked_list, p);
    }
    else {
      enqueue_fifo(&dram_cold_list, p);
    }
    pthread_mutex_unlock(&p->page_lock);
    
    p = dequeue_fifo(&dram_hot_list);
  }
  p = dequeue_fifo(&nvm_hot_list);
  while (p != NULL) {
    pthread_mutex_lock(&p->page_lock);
    assert(!p->in_dram);
    p->hot = false;
    p->accesses[DRAMREAD] = p->accesses[NVMREAD] = p->accesses[WRITE] = 0;

    if (p->stop_migrating) {
      enqueue_fifo(&nvm_locked_list, p);
    }
    else {
      enqueue_fifo(&nvm_cold_list, p);
    }
    
    pthread_mutex_unlock(&p->page_lock);
    p = dequeue_fifo(&nvm_hot_list);
  }
}


void *pebs_kscand()
{
  for(;;) {
    for (int i = 0; i < PEBS_NPROCS; i++) {
      for(int j = 0; j < NPBUFTYPES; j++) {
        struct perf_event_mmap_page *p = perf_page[i][j];
        char *pbuf = (char *)p + p->data_offset;

        __sync_synchronize();

        if(p->data_head == p->data_tail) {
          continue;
        }

        in_kscand = true;
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
                
                if (page->va != 0) {
                  page->accesses[j]++;
                  if (page->accesses[WRITE] >= HOT_WRITE_THRESHOLD) {
                    make_hot(page);
                  }
                  else if (page->accesses[DRAMREAD] + page->accesses[NVMREAD] >= HOT_READ_THRESHOLD) {
                    make_hot(page);
                  }
                }
                
                pthread_mutex_unlock(&(page->page_lock));
                hemem_pages_cnt++;
              }
              else {
                other_pages_cnt++;
              }
            
              total_pages_cnt++;
            }
	      }
  	      break;
        case PERF_RECORD_THROTTLE:
        case PERF_RECORD_UNTHROTTLE:
          //fprintf(stderr, "%s event!\n",
          //   ph->type == PERF_RECORD_THROTTLE ? "THROTTLE" : "UNTHROTTLE");
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

        in_kscand = false;
      }
    }
  }
}


static void pebs_migrate_down(struct hemem_page *page, uint64_t offset)
{
  struct timeval start, end;

  gettimeofday(&start, NULL);

  page->migrating = true;
  hemem_wp_page(page, true);
  hemem_migrate_down(page, offset);
  page->migrating = false; 

  gettimeofday(&end, NULL);
  LOG_TIME("migrate_down: %f s\n", elapsed(&start, &end));
}

static void pebs_migrate_up(struct hemem_page *page, uint64_t offset)
{
  struct timeval start, end;

  gettimeofday(&start, NULL);

  page->migrating = true;
  hemem_wp_page(page, true);
  hemem_migrate_up(page, offset);
  page->migrating = false;

  gettimeofday(&end, NULL);
  LOG_TIME("migrate_up: %f s\n", elapsed(&start, &end));
}


void *pebs_kswapd()
{
  int tries;
  struct hemem_page *p;
  struct hemem_page *cp;
  struct hemem_page *np;
  struct timeval start, end;
  uint64_t migrated_bytes;
  uint64_t old_offset;

  //free(malloc(65536));
  
  for (;;) {
    usleep(KSWAPD_INTERVAL);

    pthread_mutex_lock(&global_lock);

    gettimeofday(&start, NULL);


    // move each hot NVM page to DRAM
    for (migrated_bytes = 0; migrated_bytes < KSWAPD_MIGRATE_RATE;) {
      p = dequeue_fifo(&nvm_hot_list);
      if (p == NULL) {
        // nothing in NVM is currently hot -- bail out
        break;
      }

      pthread_mutex_lock(&(p->page_lock));

      if (p->stop_migrating) {
        // we have decided to stop migrating this page
        enqueue_fifo(&nvm_locked_list, p);
        pthread_mutex_unlock(&(p->page_lock));
        continue;
      }

      if ((p->migrations_up >= MIGRATION_STOP_THRESHOLD) || (p->migrations_down >= MIGRATION_STOP_THRESHOLD)) {
        // we have migrated this page too much -- keep it where it is
        p->stop_migrating = true;
        locked_pages++;
        enqueue_fifo(&nvm_locked_list, p);
        pthread_mutex_unlock(&(p->page_lock));
        continue;
      }
      
      for (tries = 0; tries < 2; tries++) {
        // find a free DRAM page
        np = dequeue_fifo(&dram_free_list);

        if (np != NULL) {
          pthread_mutex_lock(&(np->page_lock));

          assert(!(np->present));

          LOG("%lx: cold %lu -> hot %lu\t slowmem.hot: %lu, slowmem.cold: %lu\t fastmem.hot: %lu, fastmem.cold: %lu\n",
                p->va, p->devdax_offset, np->devdax_offset, nvm_hot_list.numentries, nvm_cold_list.numentries, dram_hot_list.numentries, dram_cold_list.numentries);

          old_offset = p->devdax_offset;
          pebs_migrate_up(p, np->devdax_offset);
          np->devdax_offset = old_offset;
          np->in_dram = false;
          np->present = false;

          enqueue_fifo(&dram_hot_list, p);
          enqueue_fifo(&nvm_free_list, np);

          migrated_bytes += pt_to_pagesize(p->pt);

          pthread_mutex_unlock(&(np->page_lock));

          break;
        }

        // no free dram page, try to find a cold dram page to move down
        cp = dequeue_fifo(&dram_cold_list);
        if (cp == NULL) {
          // all dram pages are hot, so put it back in list we got it from
          enqueue_fifo(&nvm_hot_list, p);
          pthread_mutex_unlock(&(p->page_lock));
          goto out;
        }
        assert(cp != NULL);

        pthread_mutex_lock(&(cp->page_lock));

        // find a free nvm page to move the cold dram page to
        np = dequeue_fifo(&nvm_free_list);
        if (np != NULL) {
          pthread_mutex_lock(&(np->page_lock));
          assert(!(np->present));

          LOG("%lx: hot %lu -> cold %lu\t slowmem.hot: %lu, slowmem.cold: %lu\t fastmem.hot: %lu, fastmem.cold: %lu\n",
                cp>va, cp->devdax_offset, np->devdax_offset, nvm_hot_list.numentries, nvm_cold_list.numentries, dram_hot_list.numentries, dram_cold_list.numentries);

          old_offset = cp->devdax_offset;
          pebs_migrate_down(cp, np->devdax_offset);
          np->devdax_offset = old_offset;
          np->in_dram = true;
          np->present = false;

          enqueue_fifo(&nvm_cold_list, cp);
          enqueue_fifo(&dram_free_list, np);

          pthread_mutex_unlock(&(np->page_lock));
        }
        assert(np != NULL);

        pthread_mutex_unlock(&(cp->page_lock));
      }

      pthread_mutex_unlock(&(p->page_lock));
    }

out:
    make_cold();

    pebs_runs++;
    pthread_mutex_unlock(&global_lock);
    gettimeofday(&end, NULL);
    LOG_TIME("migrate: %f s\n", elapsed(&start, &end));
  }

  return NULL;
}


/*  called with global lock held via pebs_pagefault function */
static struct hemem_page* pebs_allocate_page()
{
  struct timeval start, end;
  struct hemem_page *page;

  gettimeofday(&start, NULL);
  page = dequeue_fifo(&dram_free_list);
  if (page != NULL) {
    pthread_mutex_lock(&(page->page_lock));
    assert(page->in_dram);
    assert(!page->present);

    page->present = true;
    enqueue_fifo(&dram_cold_list, page);

    gettimeofday(&end, NULL);
    LOG_TIME("mem_policy_allocate_page: %f s\n", elapsed(&start, &end));

    pthread_mutex_unlock(&(page->page_lock));

    return page;
  }
    
  // DRAM is full, fall back to NVM
  page = dequeue_fifo(&nvm_free_list);
  if (page != NULL) {
    pthread_mutex_lock(&(page->page_lock));

    assert(!page->in_dram);
    assert(!page->present);

    page->present = true;
    enqueue_fifo(&nvm_cold_list, page);


    gettimeofday(&end, NULL);
    LOG_TIME("mem_policy_allocate_page: %f s\n", elapsed(&start, &end));

    pthread_mutex_unlock(&(page->page_lock));

    return page;
  }

  assert(!"Out of memory");
}


struct hemem_page* pebs_pagefault(void)
{
  struct hemem_page *page;

  pthread_mutex_lock(&global_lock);
  // do the heavy lifting of finding the devdax file offset to place the page
  page = pebs_allocate_page();
  pthread_mutex_unlock(&global_lock);
  assert(page != NULL);

  return page;
}

struct hemem_page* pebs_pagefault_unlocked(void)
{
  struct hemem_page *page;

  page = pebs_allocate_page();
  assert(page != NULL);

  return page;
}

void pebs_remove_page(struct hemem_page *page)
{
  struct fifo_list *list;


  // wait for kscand thread to complete its scan
  // this is needed to avoid race conditions with kscand thread
  while (in_kscand);
 
  assert(page != NULL);
  pthread_mutex_lock(&(page->page_lock));

  LOG("pebs: remove page: va: 0x%lx\n", page->va);
  
  list = page->list;
  assert(list != NULL);

  page_list_remove_page(list, page);
  page->present = false;
  page->stop_migrating = false;

  if (page->in_dram) {
    enqueue_fifo(&dram_free_list, page);
  }
  else {
    enqueue_fifo(&nvm_free_list, page);
  }

  pthread_mutex_unlock(&(page->page_lock));
}


void pebs_init(void)
{
  pthread_t kswapd_thread;
  pthread_t scan_thread;

  LOG("pebs_init: started\n");

  for (int i = 0; i < PEBS_NPROCS; i++) {
    //perf_page[i][READ] = perf_setup(0x1cd, 0x4, i);  // MEM_TRANS_RETIRED.LOAD_LATENCY_GT_4
    //perf_page[i][READ] = perf_setup(0x81d0, 0, i);   // MEM_INST_RETIRED.ALL_LOADS
    perf_page[i][DRAMREAD] = perf_setup(0x1d3, 0, i);      // MEM_LOAD_L3_MISS_RETIRED.LOCAL_DRAM
    perf_page[i][NVMREAD] = perf_setup(0x80d1, 0, i);     // MEM_LOAD_RETIRED.LOCAL_PMM
    perf_page[i][WRITE] = perf_setup(0x82d0, 0, i);    // MEM_INST_RETIRED.ALL_STORES
    //perf_page[i][WRITE] = perf_setup(0x12d0, 0, i);   // MEM_INST_RETIRED.STLB_MISS_STORES
  }

  pthread_mutex_init(&(dram_free_list.list_lock), NULL);
  for (int i = 0; i < DRAMSIZE / PAGE_SIZE; i++) {
    struct hemem_page *p = calloc(1, sizeof(struct hemem_page));
    p->devdax_offset = i * PAGE_SIZE;
    p->present = false;
    p->in_dram = true;
    p->stop_migrating = false;
    p->pt = pagesize_to_pt(PAGE_SIZE);
    pthread_mutex_init(&(p->page_lock), NULL);

    enqueue_fifo(&dram_free_list, p);
  }

  pthread_mutex_init(&(nvm_free_list.list_lock), NULL);
  for (int i = 0; i < NVMSIZE / PAGE_SIZE; i++) {
    struct hemem_page *p = calloc(1, sizeof(struct hemem_page));
    p->devdax_offset = i * PAGE_SIZE;
    p->present = false;
    p->in_dram = false;
    p->stop_migrating = false;
    p->pt = pagesize_to_pt(PAGE_SIZE);
    pthread_mutex_init(&(p->page_lock), NULL);

    enqueue_fifo(&nvm_free_list, p);
  }

  int r = pthread_create(&scan_thread, NULL, pebs_kscand, NULL);
  assert(r == 0);
  
  r = pthread_create(&kswapd_thread, NULL, pebs_kswapd, NULL);
  assert(r == 0);
  
  LOG("Memory management policy is PEBS\n");

  LOG("pebs_init: finished\n");

}

void pebs_stats()
{
  LOG_STATS("\tdram_hot_list.numentries: [%ld]\tdram_cold_list.numentries: [%ld]\tnvm_hot_list.numentries: [%ld]\tnvm_cold_list.numentries: [%ld]\themem_pages: [%lu]\ttotal_pages: [%lu]\tlocked_pages: [%ld]\tthrottle/unthrottle_cnt: [%ld/%ld]\n",
          dram_hot_list.numentries,
          dram_cold_list.numentries,
          nvm_hot_list.numentries,
          nvm_cold_list.numentries,
          hemem_pages_cnt,
          total_pages_cnt,
          locked_pages,
          throttle_cnt,
          unthrottle_cnt);
  hemem_pages_cnt = total_pages_cnt = throttle_cnt = unthrottle_cnt = 0;
}


void pebs_lock()
{
  pthread_mutex_lock(&global_lock);
}

void pebs_unlock()
{
  pthread_mutex_unlock(&global_lock);
}

