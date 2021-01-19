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


static struct pebs_list dram_hot_list;
static struct pebs_list dram_cold_list;
static struct pebs_list nvm_hot_list;
static struct pebs_list nvm_cold_list;
static struct pebs_list dram_free_list;
static struct pebs_list nvm_free_list;
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

static void pebs_list_add(struct pebs_list *list, struct pebs_node *node)
{
  pthread_mutex_lock(&(list->list_lock));
  assert(node->prev == NULL);
  node->next = list->first;
  if (list->first != NULL) {
    assert(list->first->prev == NULL);
    list->first->prev = node;
  }
  else {
    assert(list->last == NULL);
    assert(list->numentries == 0);
    list->last = node;
  }

  list->first = node;
  node->list = list;
  list->numentries++;
  pthread_mutex_unlock(&(list->list_lock));
}


static struct pebs_node* pebs_list_remove(struct pebs_list *list)
{
  pthread_mutex_lock(&(list->list_lock));
  struct pebs_node *ret = list->last;

  if (ret == NULL) {
    assert(list->numentries == 0);
    pthread_mutex_unlock(&(list->list_lock));
    return ret;
  }


  list->last = ret->prev;
  if (list->last != NULL) {
    list->last->next = NULL;
  }
  else {
    list->first = NULL;
  }

  ret->prev = NULL;
  ret->next = NULL;
  ret->list = NULL;
  assert(list->numentries > 0);
  list->numentries--;
  pthread_mutex_unlock(&(list->list_lock));
  return ret;
}

static void pebs_list_remove_node(struct pebs_list *list, struct pebs_node *node)
{
  pthread_mutex_lock(&(list->list_lock));
  if (list->first == NULL) {
    assert(list->last == NULL);
    assert(list->numentries == 0);
    pthread_mutex_unlock(&(list->list_lock));
    LOG("pebs_list_remove_node: list was empty!\n");
    return;
  }

  if (list->first == node) {
    list->first = node->next;
  }

  if (list->last == node) {
    list->last = node->prev;
  }

  if (node->next != NULL) {
    node->next->prev = node->prev;
  }

  if (node->prev != NULL) {
    node->prev->next = node->next;
  }

  assert(list->numentries > 0);
  list->numentries--;
  node->next = NULL;
  node->prev = NULL;
  node->list = NULL;
  pthread_mutex_unlock(&(list->list_lock));
}

// assumes page lock is held
void make_hot(struct hemem_page* page)
{
  assert(page != NULL);
  assert(page->va != 0);
  
  struct pebs_node *node = page->management;
  assert(node != NULL);


  if (page->hot) {
    if (page->in_dram) {
      assert(node->list == &dram_hot_list);
    }
    else {
      assert(node->list == &nvm_hot_list);
    }
    // page is already in hot list for memory type, nothing to do
    return;
  }

  if (page->in_dram) {
    assert(node->list == &dram_cold_list);
    pebs_list_remove_node(&dram_cold_list, node);
    pebs_list_add(&dram_hot_list, node);
    page->hot = true;
  }
  else {
    assert(node->list == &nvm_cold_list);
    pebs_list_remove_node(&nvm_cold_list, node);
    pebs_list_add(&nvm_hot_list, node);
    page->hot = true;
  }
}

void make_cold()
{
  struct pebs_node *n;
  struct hemem_page *p;

  n = pebs_list_remove(&dram_hot_list);
  while (n != NULL) {
    p = n->page;
    assert(p != NULL);

    pthread_mutex_lock(&p->page_lock);
    assert(p->in_dram);
    p->hot = false;
    p->accesses[DRAMREAD] = p->accesses[NVMREAD] = p->accesses[WRITE] = 0;
    pthread_mutex_unlock(&p->page_lock);

    pebs_list_add(&dram_cold_list, n);
    n = pebs_list_remove(&dram_hot_list);
  }
  n = pebs_list_remove(&nvm_hot_list);
  while (n != NULL) {
    p = n->page;
    assert(p != NULL);

    pthread_mutex_lock(&p->page_lock);
    assert(!p->in_dram);
    p->hot = false;
    p->accesses[DRAMREAD] = p->accesses[NVMREAD] = p->accesses[WRITE] = 0;
    pthread_mutex_unlock(&p->page_lock);

    pebs_list_add(&nvm_cold_list, n);
    n = pebs_list_remove(&nvm_hot_list);
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
                  if (page->accesses[DRAMREAD] + page->accesses[NVMREAD] >= HOT_READ_THRESHOLD) {
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


static void pebs_migrate_down(struct pebs_node *n, uint64_t i)
{
  struct timeval start, end;

  gettimeofday(&start, NULL);

  LOG("hemem: pebs_migrate_down: migrating %lx to NVM frame %lu\n", n->page->va, i);
  n->page->migrating = true;
  hemem_wp_page(n->page, true);
  hemem_migrate_down(n->page, i * PAGE_SIZE);
  n->page->migrating = false; 
  LOG("hemem: pebs_migrate_down: done migrating to NVM\n");

  gettimeofday(&end, NULL);
  LOG_TIME("migrate_down: %f s\n", elapsed(&start, &end));
}

static void pebs_migrate_up(struct pebs_node *n, uint64_t i)
{
  struct timeval start, end;

  gettimeofday(&start, NULL);

  LOG("hemem: pebs_migrate_up: migrating %lx to DRAM frame %lu\n", n->page->va, i);
  n->page->migrating = true;
  hemem_wp_page(n->page, true);
  hemem_migrate_up(n->page, i * PAGE_SIZE);
  n->page->migrating = false;
  LOG("hemem: pebs_migrate_up: done migrating to DRAM\n");

  gettimeofday(&end, NULL);
  LOG_TIME("migrate_up: %f s\n", elapsed(&start, &end));
}


void *pebs_kswapd()
{
  int tries;
  struct pebs_node *n;
  struct pebs_node *cn;
  struct pebs_node *nn;
  struct timeval start, end;
  uint64_t migrated_bytes;
  struct pebs_node *bookmark;

  //free(malloc(65536));
  
  for (;;) {
    usleep(KSWAPD_INTERVAL);

    pthread_mutex_lock(&global_lock);

    gettimeofday(&start, NULL);

    bookmark = NULL;

    // move each hot NVM page to DRAM
    for (migrated_bytes = 0; migrated_bytes < KSWAPD_MIGRATE_RATE;) {
      n = pebs_list_remove(&nvm_hot_list);
      if (n == NULL || n == bookmark) {
        // nothing in NVM is currently hot -- bail out
        break;
      }

      struct hemem_page *p1 = n->page;
      pthread_mutex_lock(&(p1->page_lock));

      if (p1->stop_migrating) {
        // we have decided to stop migrating this page
        pebs_list_add(&nvm_hot_list, n);
        pthread_mutex_unlock(&(p1->page_lock));
        if (bookmark == NULL) {
          bookmark = n;
        }
        continue;
      }

      if ((p1->migrations_up >= MIGRATION_STOP_THRESHOLD) || (p1->migrations_down >= MIGRATION_STOP_THRESHOLD)) {
        // we have migrated this page too much -- keep it where it is
        p1->stop_migrating = true;
        locked_pages++;
        pebs_list_add(&nvm_hot_list, n);
        pthread_mutex_unlock(&(p1->page_lock));
        if (bookmark == NULL) {
          bookmark = n;
        }
        continue;
      }
      
      for (tries = 0; tries < 2; tries++) {
        // find a free DRAM page
        nn = pebs_list_remove(&dram_free_list);

        if (nn != NULL) {
          struct hemem_page *p2 = nn->page;
          pthread_mutex_lock(&(p2->page_lock));

          assert(!(nn->page->present));

          LOG("%lx: cold %lu -> hot %lu\t slowmem.hot: %lu, slowmem.cold: %lu\t fastmem.hot: %lu, fastmem.cold: %lu\n",
                n->page->va, n->framenum, nn->framenum, nvm_hot_list.numentries, nvm_cold_list.numentries, dram_hot_list.numentries, dram_cold_list.numentries);

          pebs_migrate_up(n, nn->framenum);
          struct hemem_page *tmp;
          tmp = nn->page;
          nn->page = n->page;
          nn->page->management = nn;
          nn->page->present = true;

          n->page = tmp;
          n->page->management = n;

          n->page->devdax_offset = n->framenum * PAGE_SIZE;
          n->page->in_dram = false;
          assert(!(n->page->present));

          pebs_list_add(&dram_hot_list, nn);

          pebs_list_add(&nvm_free_list, n);

          migrated_bytes += pt_to_pagesize(nn->page->pt);

          pthread_mutex_unlock(&(p2->page_lock));

          break;
        }

        // no free dram page, try to find a cold dram page to move down
        cn = pebs_list_remove(&dram_cold_list);
        if (cn == NULL) {
          // all dram pages are hot, so put it back in list we got it from
          pebs_list_add(&nvm_hot_list, n);
          pthread_mutex_unlock(&(p1->page_lock));
          goto out;
        }
        assert(cn != NULL);

        struct hemem_page *p3 = cn->page;
        pthread_mutex_lock(&(p3->page_lock));

        // find a free nvm page to move the cold dram page to
        nn = pebs_list_remove(&nvm_free_list);
        if (nn != NULL) {
          struct hemem_page *p4 = nn->page;
          pthread_mutex_lock(&(p4->page_lock));
          assert(!(nn->page->present));

          LOG("%lx: hot %lu -> cold %lu\t slowmem.hot: %lu, slowmem.cold: %lu\t fastmem.hot: %lu, fastmem.cold: %lu\n",
                cn->page->va, cn->framenum, nn->framenum, nvm_hot_list.numentries, nvm_cold_list.numentries, dram_hot_list.numentries, dram_cold_list.numentries);

          pebs_migrate_down(cn, nn->framenum);
          struct hemem_page *tmp;
          tmp = nn->page;
          nn->page = cn->page;
          nn->page->management = nn;
          nn->page->present = true;

          cn->page = tmp;
          cn->page->management = cn;
          cn->page->devdax_offset = cn->framenum * PAGE_SIZE;
          cn->page->in_dram = true;
          assert(!(cn->page->present));

          pebs_list_add(&nvm_cold_list, nn);

          pebs_list_add(&dram_free_list, cn);

          pthread_mutex_unlock(&(p4->page_lock));
        }
        assert(nn != NULL);

        pthread_mutex_unlock(&(p3->page_lock));
      }

      pthread_mutex_unlock(&(p1->page_lock));
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
  struct pebs_node *node;

  gettimeofday(&start, NULL);
  node = pebs_list_remove(&dram_free_list);
  if (node != NULL) {
    pthread_mutex_lock(&(node->page->page_lock));
    assert(node->page->in_dram);
    assert(!node->page->present);
    assert(node->page->devdax_offset == node->framenum * PAGE_SIZE);

    node->page->present = true;
    pebs_list_add(&dram_cold_list, node);

    node->page->management = node;

    gettimeofday(&end, NULL);
    LOG_TIME("mem_policy_allocate_page: %f s\n", elapsed(&start, &end));

    pthread_mutex_unlock(&(node->page->page_lock));

    return node->page;
  }
    
  // DRAM is full, fall back to NVM
  node = pebs_list_remove(&nvm_free_list);
  if (node != NULL) {
    pthread_mutex_lock(&(node->page->page_lock));

    assert(!node->page->in_dram);
    assert(!node->page->present);
    assert(node->page->devdax_offset == node->framenum * PAGE_SIZE);

    node->page->present = true;
    pebs_list_add(&nvm_cold_list, node);

    node->page->management = node;

    gettimeofday(&end, NULL);
    LOG_TIME("mem_policy_allocate_page: %f s\n", elapsed(&start, &end));

    pthread_mutex_unlock(&(node->page->page_lock));

    return node->page;
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
  struct pebs_node *node;
  struct pebs_list *list;


  // wait for kscand thread to complete its scan
  // this is needed to avoid race conditions with kscand thread
  while (in_kscand);
 
  assert(page != NULL);
  pthread_mutex_lock(&(page->page_lock));

  LOG("pebs: remove page: va: 0x%lx\n", page->va);
  
  node = page->management;  
  assert(node != NULL);

  list = node->list;
  assert(list != NULL);

  pebs_list_remove_node(list, node);
  page->present = false;
  page->stop_migrating = false;

  if (page->in_dram) {
    pebs_list_add(&dram_free_list, node);
  }
  else {
    pebs_list_add(&nvm_free_list, node);
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
    struct pebs_node *n = calloc(1, sizeof(struct pebs_node));

    n->framenum = i;

    struct hemem_page *p = calloc(1, sizeof(struct hemem_page));
    p->devdax_offset = i * PAGE_SIZE;
    p->present = false;
    p->in_dram = true;
    p->stop_migrating = false;
    p->pt = pagesize_to_pt(PAGE_SIZE);
    pthread_mutex_init(&(p->page_lock), NULL);

    n->page = p;
    p->management = n;
    pebs_list_add(&dram_free_list, n);
  }

  pthread_mutex_init(&(nvm_free_list.list_lock), NULL);
  for (int i = 0; i < NVMSIZE / PAGE_SIZE; i++) {
    struct pebs_node *n = calloc(1, sizeof(struct pebs_node));
    
    n->framenum = i;

    struct hemem_page *p = calloc(1, sizeof(struct hemem_page));
    p->devdax_offset = i * PAGE_SIZE;
    p->present = false;
    p->in_dram = false;
    p->stop_migrating = false;
    p->pt = pagesize_to_pt(PAGE_SIZE);
    pthread_mutex_init(&(p->page_lock), NULL);

    n->page = p;
    p->management = n;
    pebs_list_add(&nvm_free_list, n);
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

