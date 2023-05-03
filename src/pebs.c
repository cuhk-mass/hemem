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
#include <sched.h>
#include <sys/ioctl.h>

#include "hemem.h"
#include "pebs.h"
#include "timer.h"
#include "spsc-ring.h"

static struct fifo_list dram_hot_list;
static struct fifo_list dram_cold_list;
static struct fifo_list nvm_hot_list;
static struct fifo_list nvm_cold_list;
static struct fifo_list dram_free_list;
static struct fifo_list nvm_free_list;
static ring_handle_t hot_ring;
static ring_handle_t cold_ring;
static ring_handle_t free_page_ring;
static pthread_mutex_t free_page_ring_lock = PTHREAD_MUTEX_INITIALIZER;
uint64_t global_clock = 0;

uint64_t hemem_pages_cnt = 0;
uint64_t other_pages_cnt = 0;
uint64_t total_pages_cnt = 0;
uint64_t zero_pages_cnt = 0;
uint64_t throttle_cnt = 0;
uint64_t unthrottle_cnt = 0;
uint64_t cools = 0;
uint64_t nsamples[NPBUFTYPES] = {};

static struct perf_event_mmap_page *perf_page[PEBS_NPROCS][NPBUFTYPES];
int pfd[PEBS_NPROCS][NPBUFTYPES];

volatile bool need_cool_dram = false;
volatile bool need_cool_nvm = false;

static long perf_event_open(struct perf_event_attr *hw_event, pid_t pid, 
    int cpu, int group_fd, unsigned long flags)
{
  int ret;

  ret = syscall(__NR_perf_event_open, hw_event, pid, cpu,
		group_fd, flags);
  return ret;
}

static struct perf_event_mmap_page* perf_setup(__u64 config, __u64 config1, __u64 cpu, __u64 type)
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

  pfd[cpu][type] = perf_event_open(&attr, -1, cpu, -1, 0);
  if(pfd[cpu][type] == -1) {
    perror("perf_event_open");
  }
  assert(pfd[cpu][type] != -1);

  size_t mmap_size = sysconf(_SC_PAGESIZE) * PERF_PAGES;
  /* printf("mmap_size = %zu\n", mmap_size); */
  struct perf_event_mmap_page *p = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, pfd[cpu][type], 0);
  if(p == MAP_FAILED) {
    perror("mmap");
  }
  assert(p != MAP_FAILED);

  return p;
}

void make_hot_request(struct hemem_page* page)
{
   page->ring_present = true;
   ring_buf_put(hot_ring, (uint64_t*)page); 
}

void make_cold_request(struct hemem_page* page)
{
    page->ring_present = true;
    ring_buf_put(cold_ring, (uint64_t*)page);
}

void *pebs_scan_thread()
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

  for(uint64_t z = 0;;++z) {
    for (int i = 0; i < PEBS_NPROCS; i++) {
      for(int j = 0; j < NPBUFTYPES; j++) {
        struct perf_event_mmap_page *p = perf_page[i][j];
        char *pbuf = (char *)p + p->data_offset;

        __sync_synchronize();

        if(p->data_head == p->data_tail) {
          continue;
        }

        struct perf_event_header *ph = (void *)(pbuf + (p->data_tail % p->data_size));
        struct perf_sample* ps;
        struct hemem_page* page;

        switch(ph->type) {
        case PERF_RECORD_SAMPLE:
            nsamples[j]++;
            ps = (struct perf_sample*)ph;
            assert(ps != NULL);
            if(ps->addr != 0) {
              __u64 pfn = ps->addr & HUGE_PFN_MASK;
            
              page = get_hemem_page(pfn);
              if (page != NULL) {
                if (page->va != 0) {
                  page->accesses[j]++;
                  page->tot_accesses[j]++;
                  if (page->accesses[WRITE] >= HOT_WRITE_THRESHOLD) {
                    if (!page->hot || !page->ring_present) {
                        make_hot_request(page);
                    }
                  }
                  else if (page->accesses[DRAMREAD] + page->accesses[NVMREAD] >= HOT_READ_THRESHOLD) {
                    if (!page->hot || !page->ring_present) {
                        make_hot_request(page);
                    }
                  }
                  else if ((page->accesses[WRITE] < HOT_WRITE_THRESHOLD) && (page->accesses[DRAMREAD] + page->accesses[NVMREAD] < HOT_READ_THRESHOLD)) {
                    if (page->hot || !page->ring_present) {
                        make_cold_request(page);
                    }
                 }

                  page->accesses[j] >>= (global_clock - page->local_clock);
                  page->local_clock = global_clock;
                  if (page->accesses[j] > PEBS_COOLING_THRESHOLD) {
                    global_clock++;
                    need_cool_dram = true;
                    need_cool_nvm = true;
                  }
                }
                hemem_pages_cnt++;
              }
              else {
                other_pages_cnt++;
              }
            
              total_pages_cnt++;
            }
            else {
              zero_pages_cnt++;
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
    if (!(z % (16L << 20))) pebs_stats();
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

// moves page to hot list -- called by migrate thread
void make_hot(struct hemem_page* page)
{
  assert(page != NULL);
  assert(page->va != 0);

  if (page->hot) {
    if (page->in_dram) {
      assert(page->list == &dram_hot_list);
    }
    else {
      assert(page->list == &nvm_hot_list);
    }

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
}

// moves page to cold list -- called by migrate thread
void make_cold(struct hemem_page* page)
{
  assert(page != NULL);
  assert(page->va != 0);

  if (!page->hot) {
    if (page->in_dram) {
      assert(page->list == &dram_cold_list);
    }
    else {
      assert(page->list == &nvm_cold_list);
    }

    return;
  }

  if (page->in_dram) {
    assert(page->list == &dram_hot_list);
    page_list_remove_page(&dram_hot_list, page);
    page->hot = false;
    enqueue_fifo(&dram_cold_list, page);
  }
  else {
    assert(page->list == &nvm_hot_list);
    page_list_remove_page(&nvm_hot_list, page);
    page->hot = false;
    enqueue_fifo(&nvm_cold_list, page);
  }
}


struct hemem_page* partial_cool_peek_and_move(struct fifo_list *hot, struct fifo_list *cold, bool dram, struct hemem_page* current)
{
  struct hemem_page *p;
  uint64_t tmp_accesses[NPBUFTYPES];
  static struct hemem_page* start_dram_page = NULL;
  static struct hemem_page* start_nvm_page = NULL;

  if (dram && !need_cool_dram) {
      return current;
  }
  if (!dram && !need_cool_nvm) {
      return current;
  }

  if (start_dram_page == NULL && dram) {
      start_dram_page = hot->last;
  }

  if (start_nvm_page == NULL && !dram) {
      start_nvm_page = hot->last;
  }

  for (int i = 0; i < COOLING_PAGES; i++) {
    p = next_page(hot, current);
    if (p == NULL) {
        break;
    }
    if (dram) {
        assert(p->in_dram);
    }
    else {
      assert(!p->in_dram);
    }

    for (int j = 0; j < NPBUFTYPES; j++) {
        tmp_accesses[j] = p->accesses[j] >> (global_clock - p->local_clock);
    }

    if ((tmp_accesses[WRITE] < HOT_WRITE_THRESHOLD) && (tmp_accesses[DRAMREAD] + tmp_accesses[NVMREAD] < HOT_READ_THRESHOLD)) {
        p->hot = false;
    }
    
    if (dram && (p == start_dram_page)) {
        start_dram_page = NULL;
        need_cool_dram = false;
    }

    if (!dram && (p == start_nvm_page)) {
        start_nvm_page = NULL;
        need_cool_nvm = false;
    } 

    if (!p->hot) {
        current = p->next;
        page_list_remove_page(hot, p);
        enqueue_fifo(cold, p);
    }
    else {
        current = p;
    }
  }

  return current;
}

void update_current_cool_page(struct hemem_page** cur_cool_in_dram, struct hemem_page** cur_cool_in_nvm, struct hemem_page* page)
{
    if (page == NULL) {
        return;
    }

    if (page == *cur_cool_in_dram) {
        assert(page->list == &dram_hot_list);
        *cur_cool_in_dram = next_page(page->list, page);
    }
    if (page == *cur_cool_in_nvm) {
        assert(page->list == &nvm_hot_list);
        *cur_cool_in_nvm = next_page(page->list, page);
    }
}

void *pebs_policy_thread()
{
  cpu_set_t cpuset;
  pthread_t thread;
  int tries;
  struct hemem_page *p;
  struct hemem_page *cp;
  struct hemem_page *np;
  uint64_t migrated_bytes;
  uint64_t old_offset;
  int num_ring_reqs;
  struct hemem_page* page = NULL;
  struct hemem_page* cur_cool_in_dram  = NULL;
  struct hemem_page* cur_cool_in_nvm = NULL;

  thread = pthread_self();
  CPU_ZERO(&cpuset);
  CPU_SET(MIGRATION_THREAD_CPU, &cpuset);
  int s = pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
  if (s != 0) {
    perror("pthread_setaffinity_np");
    assert(0);
  }

  for (;;) {
    while(!ring_buf_empty(free_page_ring)) {
        struct fifo_list *list;
        page = (struct hemem_page*)ring_buf_get(free_page_ring);
        if (page == NULL) {
            continue;
        }
        
        list = page->list;
        assert(list != NULL);
        update_current_cool_page(&cur_cool_in_dram, &cur_cool_in_nvm, page);
        page_list_remove_page(list, page);
        if (page->in_dram) {
            enqueue_fifo(&dram_free_list, page);
        }
        else {
            enqueue_fifo(&nvm_free_list, page);
        }
    }

    num_ring_reqs = 0;
    while(!ring_buf_empty(hot_ring) && num_ring_reqs < HOT_RING_REQS_THRESHOLD) {
		    page = (struct hemem_page*)ring_buf_get(hot_ring);
        if (page == NULL) {
            continue;
        }
        
        update_current_cool_page(&cur_cool_in_dram, &cur_cool_in_nvm, page);
        page->ring_present = false;
        num_ring_reqs++;
        make_hot(page);
        //printf("hot ring, hot pages:%llu\n", num_ring_reqs);
	  }

    num_ring_reqs = 0;
    while(!ring_buf_empty(cold_ring) && num_ring_reqs < COLD_RING_REQS_THRESHOLD) {
        page = (struct hemem_page*)ring_buf_get(cold_ring);
        if (page == NULL) {
            continue;
        }

        update_current_cool_page(&cur_cool_in_dram, &cur_cool_in_nvm, page);
        page->ring_present = false;
        num_ring_reqs++;
        make_cold(page);
        //printf("cold ring, cold pages:%llu\n", num_ring_reqs);
    }
    
    // move each hot NVM page to DRAM
    for (migrated_bytes = 0; migrated_bytes < PEBS_KSWAPD_MIGRATE_RATE;) {
      p = dequeue_fifo(&nvm_hot_list);
      if (p == NULL) {
        // nothing in NVM is currently hot -- bail out
        break;
      }

      update_current_cool_page(&cur_cool_in_dram, &cur_cool_in_nvm, page);
      
      if ((p->accesses[WRITE] < HOT_WRITE_THRESHOLD) && (p->accesses[DRAMREAD] + p->accesses[NVMREAD] < HOT_READ_THRESHOLD)) {
        // it has been cooled, need to move it into the cold list
        p->hot = false;
        enqueue_fifo(&nvm_cold_list, p); 
        continue;
      }

      for (tries = 0; tries < 2; tries++) {
        // find a free DRAM page
        np = dequeue_fifo(&dram_free_list);

        if (np != NULL) {
          assert(!(np->present));

          LOG("%lx: cold %lu -> hot %lu\t slowmem.hot: %lu, slowmem.cold: %lu\t fastmem.hot: %lu, fastmem.cold: %lu\n",
                p->va, p->devdax_offset, np->devdax_offset, nvm_hot_list.numentries, nvm_cold_list.numentries, dram_hot_list.numentries, dram_cold_list.numentries);

          old_offset = p->devdax_offset;
          pebs_migrate_up(p, np->devdax_offset);
          np->devdax_offset = old_offset;
          np->in_dram = false;
          np->present = false;
          np->hot = false;
          for (int i = 0; i < NPBUFTYPES; i++) {
            np->accesses[i] = 0;
            np->tot_accesses[i] = 0;
          }

          enqueue_fifo(&dram_hot_list, p);
          enqueue_fifo(&nvm_free_list, np);

          migrated_bytes += pt_to_pagesize(p->pt);
          break;
        }

        // no free dram page, try to find a cold dram page to move down
        cp = dequeue_fifo(&dram_cold_list);
        if (cp == NULL) {
          // all dram pages are hot, so put it back in list we got it from
          enqueue_fifo(&nvm_hot_list, p);
          goto out;
        }
        assert(cp != NULL);
         
        // find a free nvm page to move the cold dram page to
        np = dequeue_fifo(&nvm_free_list);
        if (np != NULL) {
          assert(!(np->present));

          LOG("%lx: hot %lu -> cold %lu\t slowmem.hot: %lu, slowmem.cold: %lu\t fastmem.hot: %lu, fastmem.cold: %lu\n",
                cp->va, cp->devdax_offset, np->devdax_offset, nvm_hot_list.numentries, nvm_cold_list.numentries, dram_hot_list.numentries, dram_cold_list.numentries);

          old_offset = cp->devdax_offset;
          pebs_migrate_down(cp, np->devdax_offset);
          np->devdax_offset = old_offset;
          np->in_dram = true;
          np->present = false;
          np->hot = false;
          for (int i = 0; i < NPBUFTYPES; i++) {
            np->accesses[i] = 0;
            np->tot_accesses[i] = 0;
          }

          enqueue_fifo(&nvm_cold_list, cp);
          enqueue_fifo(&dram_free_list, np);
        }
        assert(np != NULL);
      }
    }

    cur_cool_in_dram = partial_cool_peek_and_move(&dram_hot_list, &dram_cold_list, true, cur_cool_in_dram);
    cur_cool_in_nvm = partial_cool_peek_and_move(&nvm_hot_list, &nvm_cold_list, false, cur_cool_in_nvm);
 
out:
    LOG_TIME("migrate: %f s\n", elapsed(&start, &end));
  }

  return NULL;
}

static struct hemem_page* pebs_allocate_page()
{
  struct timeval start, end;
  struct hemem_page *page;

  gettimeofday(&start, NULL);
  page = dequeue_fifo(&dram_free_list);
  if (page != NULL) {
    assert(page->in_dram);
    assert(!page->present);

    page->present = true;
    enqueue_fifo(&dram_cold_list, page);

    gettimeofday(&end, NULL);
    LOG_TIME("mem_policy_allocate_page: %f s\n", elapsed(&start, &end));

    return page;
  }
    
  // DRAM is full, fall back to NVM
  page = dequeue_fifo(&nvm_free_list);
  if (page != NULL) {
    assert(!page->in_dram);
    assert(!page->present);

    page->present = true;
    enqueue_fifo(&nvm_cold_list, page);


    gettimeofday(&end, NULL);
    LOG_TIME("mem_policy_allocate_page: %f s\n", elapsed(&start, &end));

    return page;
  }

  assert(!"Out of memory");
}

struct hemem_page* pebs_pagefault(void)
{
  struct hemem_page *page;

  // do the heavy lifting of finding the devdax file offset to place the page
  page = pebs_allocate_page();
  assert(page != NULL);

  return page;
}

void pebs_remove_page(struct hemem_page *page)
{
  assert(page != NULL);

  LOG("pebs: remove page, put this page into free_page_ring: va: 0x%lx\n", page->va);

  pthread_mutex_lock(&free_page_ring_lock);
  while (ring_buf_full(free_page_ring));
  ring_buf_put(free_page_ring, (uint64_t*)page); 
  pthread_mutex_unlock(&free_page_ring_lock);

  page->present = false;
  page->hot = false;
  for (int i = 0; i < NPBUFTYPES; i++) {
    page->accesses[i] = 0;
    page->tot_accesses[i] = 0;
  }
}

void pebs_init(void)
{
  pthread_t kswapd_thread;
  pthread_t scan_thread;
  uint64_t** buffer;

  LOG("pebs_init: started\n");

  for (int i = 0; i < PEBS_NPROCS; i++) {
    //perf_page[i][READ] = perf_setup(0x1cd, 0x4, i);  // MEM_TRANS_RETIRED.LOAD_LATENCY_GT_4
    //perf_page[i][READ] = perf_setup(0x81d0, 0, i);   // MEM_INST_RETIRED.ALL_LOADS
    perf_page[i][DRAMREAD] = perf_setup(0x1d3, 0, i, DRAMREAD);      // MEM_LOAD_L3_MISS_RETIRED.LOCAL_DRAM
    perf_page[i][NVMREAD] = perf_setup(0x80d1, 0, i, NVMREAD);     // MEM_LOAD_RETIRED.LOCAL_PMM
    perf_page[i][WRITE] = perf_setup(0x82d0, 0, i, WRITE);    // MEM_INST_RETIRED.ALL_STORES
    //perf_page[i][WRITE] = perf_setup(0x12d0, 0, i);   // MEM_INST_RETIRED.STLB_MISS_STORES
  }

  pthread_mutex_init(&(dram_free_list.list_lock), NULL);
  for (int i = 0; i < DRAMSIZE / PAGE_SIZE; i++) {
    struct hemem_page *p = calloc(1, sizeof(struct hemem_page));
    p->devdax_offset = i * PAGE_SIZE;
    p->present = false;
    p->in_dram = true;
    p->ring_present = false;
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
    p->ring_present = false;
    p->pt = pagesize_to_pt(PAGE_SIZE);
    pthread_mutex_init(&(p->page_lock), NULL);

    enqueue_fifo(&nvm_free_list, p);
  }

  pthread_mutex_init(&(dram_hot_list.list_lock), NULL);
  pthread_mutex_init(&(dram_cold_list.list_lock), NULL);
  pthread_mutex_init(&(nvm_hot_list.list_lock), NULL);
  pthread_mutex_init(&(nvm_cold_list.list_lock), NULL);

  buffer = (uint64_t**)malloc(sizeof(uint64_t*) * CAPACITY);
  assert(buffer); 
  hot_ring = ring_buf_init(buffer, CAPACITY);
  buffer = (uint64_t**)malloc(sizeof(uint64_t*) * CAPACITY);
  assert(buffer); 
  cold_ring = ring_buf_init(buffer, CAPACITY);
  buffer = (uint64_t**)malloc(sizeof(uint64_t*) * CAPACITY);
  assert(buffer); 
  free_page_ring = ring_buf_init(buffer, CAPACITY);

  int r = pthread_create(&scan_thread, NULL, pebs_scan_thread, NULL);
  assert(r == 0);
  
  r = pthread_create(&kswapd_thread, NULL, pebs_policy_thread, NULL);
  assert(r == 0);
  
  LOG("Memory management policy is PEBS\n");

  LOG("pebs_init: finished\n");

}

void pebs_shutdown()
{
  for (int i = 0; i < PEBS_NPROCS; i++) {
    for (int j = 0; j < NPBUFTYPES; j++) {
      ioctl(pfd[i][j], PERF_EVENT_IOC_DISABLE, 0);
      //munmap(perf_page[i][j], sysconf(_SC_PAGESIZE) * PERF_PAGES);
    }
  }
}

void pebs_stats()
{
  uint64_t total_samples = nsamples[DRAMREAD] + nsamples[NVMREAD] + nsamples[WRITE];
  LOG_STATS("\tdram_hot_list.numentries: [%ld]"
	    "\tdram_cold_list.numentries: [%ld]"
	    "\tdram_free_list.numentries: [%ld]"
	    "\tnvm_hot_list.numentries: [%ld]"
	    "\tnvm_cold_list.numentries: [%ld]"
	    "\tnvm_free_list.numentries: [%ld]"
	    "\themem_pages: [%lu]"
	    "\ttotal_pages: [%lu]"
	    "\tzero_pages: [%ld]"
	    "\tthrottle/unthrottle_cnt: [%ld/%ld]"
	    "\tcools: [%ld]"
	    "\tdramread: [%ld/%ld]"
	    "\tnvmread: [%ld/%ld]"
	    "\twrite: [%ld/%ld]"
	    "\n",
          dram_hot_list.numentries,
          dram_cold_list.numentries,
          dram_free_list.numentries,
          nvm_hot_list.numentries,
          nvm_cold_list.numentries,
          nvm_free_list.numentries,
          hemem_pages_cnt,
          total_pages_cnt,
          zero_pages_cnt,
          throttle_cnt,
          unthrottle_cnt,
          cools,
          nsamples[DRAMREAD],
          total_samples,
          nsamples[NVMREAD],
          total_samples,
          nsamples[WRITE],
          total_samples);
  nsamples[DRAMREAD] = nsamples[NVMREAD] = nsamples[WRITE] = 0;
  hemem_pages_cnt = total_pages_cnt =  throttle_cnt = unthrottle_cnt = 0;
}
