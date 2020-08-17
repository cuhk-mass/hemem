#ifndef HEMEM_H

#define HEMEM_H

#include <pthread.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#ifndef __cplusplus
#include <stdatomic.h>
#else
#include <atomic>
#define _Atomic(X) std::atomic< X >
#endif

#ifdef __cplusplus
extern "C" {
#endif

#include "paging.h"
#include "hemem-mmgr.h"
#include "lru.h"
#include "simple.h"
#include "timer.h"
#include "interpose.h"
#include "uthash.h"
#include "pebs.h"

//#define HEMEM_DEBUG
//#define USE_PEBS
//#define STATS_THREAD

#define MEM_BARRIER() __sync_synchronize()

#define NVMSIZE   (450L * (1024L * 1024L * 1024L))
#define DRAMSIZE  (128L * (1024L * 1024L * 1024L))

#define DRAMPATH  "/dev/dax0.0"
#define NVMPATH   "/dev/dax1.0"

//#define PAGE_SIZE (1024 * 1024 * 1024)
//#define PAGE_SIZE (2 * (1024 * 1024))
#define BASEPAGE_SIZE	  (4UL * 1024UL)
#define HUGEPAGE_SIZE 	(2UL * 1024UL * 1024UL)
#define PAGE_SIZE 	    HUGEPAGE_SIZE

#define FASTMEM_PAGES   ((DRAMSIZE) / (PAGE_SIZE))
#define SLOWMEM_PAGES   ((NVMSIZE) / (PAGE_SIZE))

#define BASEPAGE_MASK	(BASEPAGE_SIZE - 1)
#define HUGEPAGE_MASK	(HUGEPAGE_SIZE - 1)

#define BASE_PFN_MASK	(BASEPAGE_MASK ^ UINT64_MAX)
#define HUGE_PFN_MASK	(HUGEPAGE_MASK ^ UINT64_MAX)

FILE *hememlogf;
//#define LOG(...) fprintf(stderr, __VA_ARGS__)
//#define LOG(...)	fprintf(hememlogf, __VA_ARGS__)
#define LOG(str, ...) while(0) {}

FILE *timef;
extern bool timing;

static inline void log_time(const char* fmt, ...)
{
  if (timing) {
    va_list args;
    va_start(args, fmt);
    vfprintf(timef, fmt, args);
    va_end(args);
  }
}


#define LOG_TIME(str, ...) log_time(str, __VA_ARGS__)
//#define LOG_TIME(str, ...) fprintf(timef, str, __VA_ARGS__)
//#define LOG_TIME(str, ...) while(0) {}

FILE *statsf;
#define LOG_STATS(str, ...) fprintf(stderr, str,  __VA_ARGS__)
//#define LOG_STATS(str, ...) fprintf(statsf, str, __VA_ARGS__)
//#define LOG_STATS(str, ...) while (0) {}

#if defined (ALLOC_HEMEM)
  #define pagefault(...) hemem_mmgr_pagefault(__VA_ARGS__)
  #define pagefault_unlocked(...) hemem_mmgr_pagefault_unlocked(__VA_ARGS__)
  #define paging_init(...) hemem_mmgr_init(__VA_ARGS__)
  #define mmgr_remove(...) hemem_mmgr_remove_page(__VA_ARGS__)
  #define mmgr_stats(...) hemem_mmgr_stats(__VA_ARGS__)
  #define policy_lock(...) hemem_mmgr_lock(__VA_ARGS__)
  #define policy_unlock(...) hemem_mmgr_unlock(__VA_ARGS__)
#elif defined (ALLOC_LRU)
  #define pagefault(...) lru_pagefault(__VA_ARGS__)
  #define pagefault_unlocked(...) lru_pagefault_unlocked(__VA_ARGS__)
  #define paging_init(...) lru_init(__VA_ARGS__)
  #define mmgr_remove(...) lru_remove_page(__VA_ARGS__)
  #define mmgr_stats(...) lru_stats(__VA_ARGS__)
  #define policy_lock(...) lru_lock(__VA_ARGS__)
  #define policy_unlock(...) lru_unlock(__VA_ARGS__)
#elif defined (ALLOC_SIMPLE)
  #define pagefault(...) simple_pagefault(__VA_ARGS__)
  #define pagefault_unlocked(...) simple_pagefault(__VA_ARGS__)
  #define paging_init(...) simple_init(__VA_ARGS__)
  #define mmgr_remove(...) simple_remove_page(__VA_ARGS__)
  #define mmgr_stats(...) simple_stats(__VA_ARGS__)
  #define policy_lock(...) while (0) {}
  #define policy_unlock(...) while (0) {}
#endif


#define MAX_UFFD_MSGS	    (1)
#define MAX_COPY_THREADS  (4)

extern uint64_t cr3;
extern int dramfd;
extern int nvmfd;
extern int devmemfd;
extern bool is_init;
extern uint64_t missing_faults_handled;
extern uint64_t migrations_up;
extern uint64_t migrations_down;
extern __thread bool internal_malloc;
extern __thread bool old_internal_call;
extern __thread bool internal_call;
extern __thread bool internal_munmap;
extern void* devmem_mmap;

enum memtypes {
  FASTMEM = 0,
  SLOWMEM = 1,
  NMEMTYPES,
};

enum pagetypes {
  HUGEP = 0,
  BASEP = 1,
  NPAGETYPES
};

struct hemem_page {
  uint64_t va;
  uint64_t devdax_offset;
  uint64_t *pgd, *pud, *pmd, *pte;
  uint64_t *pa;
  bool in_dram;
  enum pagetypes pt;
  bool migrating;
  bool present;
  bool written;
  uint64_t naccesses;
  pthread_mutex_t page_lock;
  uint64_t migrations_up, migrations_down;
  UT_hash_handle hh;
  void *management;

  struct hemem_page *next, *prev;
#ifdef USE_PEBS
  uint64_t accesses[NPBUFTYPES];
  UT_hash_handle phh;     // pebs hash handle
#endif
};

struct fifo_list {
  struct hemem_page *first, *last;
  pthread_mutex_t list_lock;
  size_t numentries;
};

static inline uint64_t pt_to_pagesize(enum pagetypes pt)
{
  switch(pt) {
  case HUGEP: return HUGEPAGE_SIZE;
  case BASEP: return BASEPAGE_SIZE;
  default: assert(!"Unknown page type");
  }
}

static inline enum pagetypes pagesize_to_pt(uint64_t pagesize)
{
  switch (pagesize) {
    case BASEPAGE_SIZE: return BASEP;
    case HUGEPAGE_SIZE: return HUGEP;
    default: assert(!"Unknown page ssize");
  }
}

void hemem_init();
void* hemem_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int hemem_munmap(void* addr, size_t length);
void *handle_fault();
void hemem_migrate_up(struct hemem_page *page, uint64_t dram_offset);
void hemem_migrate_down(struct hemem_page *page, uint64_t nvm_offset);
void hemem_wp_page(struct hemem_page *page, bool protect);
void hemem_promote_pages(uint64_t addr);
void hemem_demote_pages(uint64_t addr);

void hemem_clear_bits(struct hemem_page *page);
uint64_t hemem_get_bits(struct hemem_page *page);
void hemem_tlb_shootdown(uint64_t va);

struct hemem_page* get_hemem_page(uint64_t va);

void hemem_print_stats();
void hemem_clear_stats();

void enqueue_fifo(struct fifo_list *list, struct hemem_page *page);
struct hemem_page* dequeue_fifo(struct fifo_list *list);

void hemem_start_timing(void);


#ifdef __cplusplus
}
#endif

#endif /* HEMEM_H */
