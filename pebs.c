#define _GNU_SOURCE
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
static struct fifo_list dram_written_list;
static struct fifo_list nvm_hot_list;
static struct fifo_list nvm_cold_list;
static struct fifo_list nvm_locked_list;
static struct fifo_list nvm_written_list;
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
uint64_t cools = 0;

static struct perf_event_mmap_page *perf_page[PEBS_NPROCS][NPBUFTYPES];

static volatile bool needs_cooling = false;


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

static void cool(struct fifo_list *hot, struct fifo_list *cold, struct fifo_list *written, bool dram)
{
  struct hemem_page *p;
  struct hemem_page *bookmark;

  // cool written pages
  bookmark = NULL;
  p = dequeue_fifo(written);
  while (p != NULL) {
    if (p == bookmark) {
      // we've seen this age before, so put it back and bail out
      enqueue_fifo(written, p);
      break;
    }

    if (dram) {
      assert(p->in_dram);
    }
    else {
      assert(!p->in_dram);
    }

    p->accesses[WRITE]--;

    if (p->accesses[WRITE] < HOT_WRITE_THRESHOLD) {
      LOG("%lx: no longer written: %lu %lu %lu\n", p->va, p->accesses[DRAMREAD], p->accesses[NVMREAD], p->accesses[WRITE]);
      p->written = false;
    }

    if (p->written) {
      enqueue_fifo(written, p);
      if (bookmark == NULL) {
        bookmark = p;
      }
    }
    else {
      enqueue_fifo(hot, p);
    }

    p = dequeue_fifo(written);
  }

  // cool hot pages
  bookmark = NULL;
  p = dequeue_fifo(hot);
  while (p != NULL) {
    //pthread_mutex_lock(&p->page_lock);
    if (p == bookmark) {
      // we've seen this page before, so put it back and bail out
      enqueue_fifo(hot, p);
      //pthread_mutex_unlock(&p->page_lock);
      break;
    }

    if (dram) {
      assert(p->in_dram);
    }
    else {
      assert(!p->in_dram);
    }

    p->accesses[DRAMREAD] >>= 1;
    p->accesses[NVMREAD] >>= 1;

    // is page still hot?
    if (p->accesses[DRAMREAD] + p->accesses[NVMREAD] < HOT_READ_THRESHOLD) {
      LOG("%lx: became cold: %lu %lu %lu\n", p->va, p->accesses[DRAMREAD], p->accesses[NVMREAD], p->accesses[WRITE]);
      p->hot = false;
    }
    //else if (p->accesses[DRAMREAD] + p->accesses[NVMREAD] < HOT_READ_THRESHOLD) {
    //  p->hot = false;
    //}

    if (p->hot) {
      enqueue_fifo(hot, p);
      if (bookmark == NULL) {
        bookmark = p;
      }
    }
    else {
      enqueue_fifo(cold, p);
    }

    //pthread_mutex_unlock(&p->page_lock);
    p = dequeue_fifo(hot);
  }
}

void *pebs_cooling()
{
  cpu_set_t cpuset;
  pthread_t thread;

  thread = pthread_self();
  CPU_ZERO(&cpuset);
  CPU_SET(COOLING_THREAD_CPU, &cpuset);
  int s = pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
  if (s != 0) {
    perror("pthread_setaffinity_np");
    assert(0);
  }

  for(;;) {
    //usleep(PEBS_COOLING_INTERVAL);
    while (!needs_cooling);

    cool(&dram_hot_list, &dram_cold_list, &dram_written_list, true);
    cool(&nvm_hot_list, &nvm_cold_list, &nvm_written_list, false);

    cools++;

    needs_cooling = false;
  }

  return NULL;
}

// assumes page lock is held
void make_written(struct hemem_page* page)
{
  assert(page != NULL);
  assert(page->va != 0);

  if (page->stop_migrating) {
    if (page->in_dram) {
      assert(page->list == &dram_locked_list);
    }
    else {
      assert(page->list == &nvm_locked_list);
    }
    return;
  }

  if (page->written) {
    assert(page->hot);
    if (page->in_dram) {
      assert(page->list == &dram_written_list);
    }
    else {
      assert(page->list == &nvm_written_list);
    }
    return;
  }

  if (page->migrating) {
    return;
  }

  if (page->in_dram) {
    if (page->hot) {
      assert(page->list == &dram_hot_list);
      page_list_remove_page(&dram_hot_list, page);
      page->written = true;
      enqueue_fifo(&dram_written_list, page);
    }
    else {
      assert(page->list == &dram_cold_list);
      page_list_remove_page(&dram_cold_list, page);
      page->written = page->hot = true;
      enqueue_fifo(&dram_written_list, page);
    }
  }
  else {
    if (page->hot) {
      assert(page->list == &nvm_hot_list);
      page_list_remove_page(&nvm_hot_list, page);
      page->written = true;
      enqueue_fifo(&nvm_written_list, page);
    }
    else {
      assert(page->list == &nvm_cold_list);
      page_list_remove_page(&nvm_cold_list, page);
      page->written = page->hot = true;
      enqueue_fifo(&nvm_written_list, page);
    } 
  }
  LOG("%lx: became written: %lu %lu %lu\n", page->va, page->accesses[DRAMREAD], page->accesses[NVMREAD], page->accesses[WRITE]);
}


// assumes page lock is held
void make_hot(struct hemem_page* page)
{
  assert(page != NULL);
  assert(page->va != 0);

  if (page->stop_migrating) {
    if (page->in_dram) {
      assert(page->list == &dram_locked_list);
    }
    else {
      assert(page->list == &nvm_locked_list);
    }
    // we have decided to stop migrating this page
    return;
  }

  if (page->hot) {
    if (page->in_dram) {
      if (page->written) {
        assert(page->list == &dram_written_list);
      }
      else {
        assert(page->list == &dram_hot_list);
      }
    }
    else {
      if (page->written) {
        assert(page->list == &nvm_written_list);
      }
      else {
        assert(page->list == &nvm_hot_list);
      }
    }
    // page is already hot -- nothing to do
    return;
  }

  if (page->migrating) {
    return;
  }

  if (page->in_dram) {
    assert(page->list == &dram_cold_list);
    page_list_remove_page(&dram_cold_list, page);
    page->hot = true;
    enqueue_fifo(&dram_hot_list, page);
  }
  else {
    assert(page->list == &nvm_cold_list);
    page_list_remove_page(&nvm_cold_list, page);
    page->hot = true;
    enqueue_fifo(&nvm_hot_list, page);
  }
  LOG("%lx: became hot: %lu %lu %lu\n", page->va, page->accesses[DRAMREAD], page->accesses[NVMREAD], page->accesses[WRITE]);
}


void *pebs_kscand()
{
 cpu_set_t cpuset;
  pthread_t thread;

  thread = pthread_self();
  CPU_ZERO(&cpuset);
  CPU_SET(SCANNING_THREAD_CPU, &cpuset);
  int s = pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
  if (s != 0) {
    perror("pthread_setaffinity_np");
    assert(0);
  }

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
            struct perf_sample *ps = (struct perf_sample*)ph;
            assert(ps != NULL);
            if(ps->addr != 0) {
              __u64 pfn = ps->addr & HUGE_PFN_MASK;
            
              struct hemem_page* page = get_hemem_page(pfn);
              if (page != NULL) {
                pthread_mutex_lock(&(page->page_lock));
                in_kscand = true;
                if (page->va != 0) {
                  LOG("%lx: recorded PEBS access: %d\n", page->va, j);
                  page->accesses[j]++;
                  page->tot_accesses[j]++;
                  if (page->accesses[WRITE] >= HOT_WRITE_THRESHOLD) {
                    make_written(page);
                  }
                  else if (page->accesses[DRAMREAD] + page->accesses[NVMREAD] >= HOT_READ_THRESHOLD) {
                    make_hot(page);
                  }

                  if (page->accesses[j] > PEBS_COOLING_THRESHOLD) {
                    needs_cooling = true;
                  }
                }
                in_kscand = false;
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
      }
    }
  }

  return NULL;
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
  bool from_written_list;

  //free(malloc(65536));
  cpu_set_t cpuset;
  pthread_t thread;

  thread = pthread_self();
  CPU_ZERO(&cpuset);
  CPU_SET(MIGRATION_THREAD_CPU, &cpuset);
  int s = pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
  if (s != 0) {
    perror("pthread_setaffinity_np");
    assert(0);
  } 
  
  for (;;) {
    usleep(KSWAPD_INTERVAL);

    pthread_mutex_lock(&global_lock);

    gettimeofday(&start, NULL);


    // move each hot NVM page to DRAM
    for (migrated_bytes = 0; migrated_bytes < KSWAPD_MIGRATE_RATE;) {
      p = dequeue_fifo(&nvm_written_list);
      if (p == NULL) {
        p = dequeue_fifo(&nvm_hot_list);
        if (p == NULL) {
          // nothing in NVM is currently hot or written -- bail out
          break;
        }
        from_written_list = false;
      }
      else {
        from_written_list = true;
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
          np->stop_migrating = false;
          np->hot = false;
          for (int i = 0; i < NPBUFTYPES; i++) {
            np->accesses[i] = 0;
            np->tot_accesses[i] = 0;
          }

          if (from_written_list) {
            enqueue_fifo(&dram_written_list, p);
          }
          else {
            enqueue_fifo(&dram_hot_list, p);
          }
          enqueue_fifo(&nvm_free_list, np);

          migrated_bytes += pt_to_pagesize(p->pt);

          pthread_mutex_unlock(&(np->page_lock));

          break;
        }

        // no free dram page, try to find a cold dram page to move down
        cp = dequeue_fifo(&dram_cold_list);
        if (cp == NULL) {
          // all dram pages are hot, so put it back in list we got it from
          if (from_written_list) {
            enqueue_fifo(&nvm_written_list, p);
          }
          else {
            enqueue_fifo(&nvm_hot_list, p);
          }
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
                cp->va, cp->devdax_offset, np->devdax_offset, nvm_hot_list.numentries, nvm_cold_list.numentries, dram_hot_list.numentries, dram_cold_list.numentries);

          old_offset = cp->devdax_offset;
          pebs_migrate_down(cp, np->devdax_offset);
          np->devdax_offset = old_offset;
          np->in_dram = true;
          np->present = false;
          np->stop_migrating = false;
          np->hot = false;
          for (int i = 0; i < NPBUFTYPES; i++) {
            np->accesses[i] = 0;
            np->tot_accesses[i] = 0;
          }

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
  page->hot = false;
  for (int i = 0; i < NPBUFTYPES; i++) {
    page->accesses[i] = 0;
    page->tot_accesses[i] = 0;
  }

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
  pthread_t cooling_thread;

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

  pthread_mutex_init(&(dram_hot_list.list_lock), NULL);
  pthread_mutex_init(&(dram_cold_list.list_lock), NULL);
  pthread_mutex_init(&(dram_written_list.list_lock), NULL);
  pthread_mutex_init(&(dram_locked_list.list_lock), NULL);
  pthread_mutex_init(&(nvm_hot_list.list_lock), NULL);
  pthread_mutex_init(&(nvm_cold_list.list_lock), NULL);
  pthread_mutex_init(&(nvm_written_list.list_lock), NULL);
  pthread_mutex_init(&(nvm_locked_list.list_lock), NULL);

  int r = pthread_create(&scan_thread, NULL, pebs_kscand, NULL);
  assert(r == 0);
  
  r = pthread_create(&kswapd_thread, NULL, pebs_kswapd, NULL);
  assert(r == 0);

  r = pthread_create(&cooling_thread, NULL, pebs_cooling, NULL);
  assert(r == 0);
  
  LOG("Memory management policy is PEBS\n");

  LOG("pebs_init: finished\n");

}

void pebs_stats()
{
  LOG_STATS("\tdram_written: [%ld]\tdram_hot: [%ld]\tdram_cold: [%ld]\tnvm_written: [%ld]\tnvm_hot: [%ld]\tnvm_cold: [%ld]\themem_pages: [%lu]\tlocked_pages: [%ld]\tthrottle/unthrottle_cnt: [%ld/%ld]\tcools: [%ld]\n",
          dram_written_list.numentries,
          dram_hot_list.numentries,
          dram_cold_list.numentries,
          nvm_written_list.numentries,
          nvm_hot_list.numentries,
          nvm_cold_list.numentries,
          hemem_pages_cnt,
          locked_pages,
          throttle_cnt,
          unthrottle_cnt,
          cools);
  hemem_pages_cnt = total_pages_cnt = throttle_cnt = unthrottle_cnt = 0;
}


