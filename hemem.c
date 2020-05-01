#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <sys/time.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <math.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>

#include "hemem.h"
#include "timer.h"
#include "paging.h"

pthread_t fault_thread;

int dramfd = -1;
int nvmfd = -1;
long uffd = -1;
bool is_init = false;
_Atomic uint64_t mem_allocated = 0;
_Atomic uint64_t fastmem_allocated = 0;
_Atomic uint64_t slowmem_allocated = 0;
_Atomic uint64_t wp_faults_handled = 0;
_Atomic uint64_t missing_faults_handled = 0;
_Atomic uint64_t migrations_up = 0;
_Atomic uint64_t migrations_down = 0;
_Atomic uint64_t pmemcpys = 0;
_Atomic uint64_t memsets = 0;
uint64_t cr3 = 0;
int devmemfd = -1;
struct page_list list;
pthread_t copy_threads[MAX_COPY_THREADS];

struct page_list freepages;

void *dram_devdax_mmap;
void *nvm_devdax_mmap;

__thread bool internal_malloc = false;

__thread bool ignore_this_mmap = false;

struct pmemcpy {
  pthread_barrier_t barrier;
  _Atomic void *dst;
  _Atomic void *src;
  _Atomic size_t length;
};

static struct pmemcpy pmemcpy;

void *hemem_parallel_memcpy_thread(void *arg)
{
  uint64_t tid = (uint64_t)arg;
  void *src;
  void *dst;
  size_t length;
  size_t chunk_size;

  ignore_this_mmap = true;
  assert(tid < MAX_COPY_THREADS);
  ignore_this_mmap = false;

  for (;;) {
    /* while(!pmemcpy.activate || pmemcpy.done_bitmap[tid]) { } */
    int r = pthread_barrier_wait(&pmemcpy.barrier);
    ignore_this_mmap = true;
    assert(r == 0 || r == PTHREAD_BARRIER_SERIAL_THREAD);
    ignore_this_mmap = false;
    if (tid == 0) {
      pmemcpys++;
    }

    // grab data out of shared struct
    length = pmemcpy.length;
    chunk_size = length / MAX_COPY_THREADS;
    src = pmemcpy.src + (tid * chunk_size);
    dst = pmemcpy.dst + (tid * chunk_size);

    LOG("thread %lu copying %lu bytes from %lx to %lx\n", tid, chunk_size, (uint64_t)dst, (uint64_t)src);

    memcpy(dst, src, chunk_size);

#ifdef HEMEM_DEBUG
    uint64_t *tmp1, *tmp2, i;
    tmp1 = dst;
    tmp2 = src;
    for (i = 0; i < chunk_size / sizeof(uint64_t); i++) {
      if (tmp1[i] != tmp2[i]) {
        LOG("copy thread: dst[%lu] = %lu != src[%lu] = %lu\n", i, tmp1[i], i, tmp2[i]);
        ignore_this_mmap = true;
        assert(tmp1[i] == tmp2[i]);
        ignore_this_mmap = false;
      }
    }
#endif

    LOG("thread %lu done copying\n", tid);

    r = pthread_barrier_wait(&pmemcpy.barrier);
    ignore_this_mmap = true;
    assert(r == 0 || r == PTHREAD_BARRIER_SERIAL_THREAD);
    ignore_this_mmap = false;
    /* pmemcpy.done_bitmap[tid] = true; */
  }
  return NULL;
}

void enqueue_page(struct page_list *list, struct hemem_page *page)
{
  ignore_this_mmap = true;
  assert(page->prev == NULL);
  ignore_this_mmap = false;
  page->next = list->first;
  if (list->first != NULL) {
    ignore_this_mmap = true;
    assert(list->first->prev == NULL);
    ignore_this_mmap = false;
    list->first->prev = page;
  }
  else {
    ignore_this_mmap = true;
    assert(list->last == NULL);
    assert(list->numentries == 0);
    ignore_this_mmap = false;
    list->last = page;
  }
  list->first = page;
  list->numentries++;
}

struct hemem_page* dequeue_page(struct page_list *list)
{
  struct hemem_page *ret = list->last;

  if (ret == NULL) {
    ignore_this_mmap = true;
    assert(list->numentries == 0);
    ignore_this_mmap = false;
    return ret;
  }

  list->last = ret->prev;
  if (list->last != NULL) {
    list->last->next = NULL;
  }
  else {
    list->first = NULL;
  }

  ret->prev = ret->next = NULL;
  ignore_this_mmap = true;
  assert(list->numentries > 0);
  ignore_this_mmap = false;
  list->numentries--;
  return ret;
}

struct hemem_page* hemem_get_free_page()
{
  return dequeue_page(&freepages);
}

void hemem_put_free_page(struct hemem_page *page)
{
  enqueue_page(&freepages, page);
}

void remove_page(struct page_list *list, struct hemem_page *page)
{
  if (list->first == NULL) {
    ignore_this_mmap = true;
    assert(list->last == NULL);
    assert(list->numentries == 0);
    ignore_this_mmap = false;
    return;
  }

  if (list->first == page) {
    list->first = page->next;
  }

  if (page->next != NULL) {
    page->next->prev = page->prev;
  }

  if (page->prev != NULL) {
    page->prev->next = page->next;
  }

  list->numentries--;
}

struct hemem_page* find_page(struct page_list *list, uint64_t va)
{
  struct hemem_page *cur = list->first;

  while (cur != NULL) {
    if (cur->va == va) {
      return cur;
    }
    cur = cur->next;
  }

  return NULL;
}


void hemem_init()
{
  struct uffdio_api uffdio_api;
  struct uffdio_cr3 uffdio_cr3;
/*
  {
    // This call is dangerous. Ideally, all printf's should be
    // replaced with logging macros that can print to stderr instead
    // (which is unbuffered).
    int r = setvbuf(stdout, NULL, _IONBF, 0);
    assert(r == 0);
  }
*/
  
  hememlogf = fopen("logs.txt", "w+");
  if (hememlogf == NULL) {
    perror("log file open\n");
    ignore_this_mmap = true;
    assert(0);
    ignore_this_mmap = false;
  }

  LOG("hemem_init: started\n");

  // pre-allocate maximum amount of pages
  for (int i = 0; i < (DRAMSIZE + NVMSIZE) / BASEPAGE_SIZE; i++) {
    struct hemem_page *p = calloc(1, sizeof(struct hemem_page));
    enqueue_page(&freepages, p);
  }

  dramfd = open(DRAMPATH, O_RDWR);
  if (dramfd < 0) {
    perror("dram open");
  }
  ignore_this_mmap = true;
  assert(dramfd >= 0);
  ignore_this_mmap = false;

  nvmfd = open(NVMPATH, O_RDWR);
  if (nvmfd < 0) {
    perror("nvm open");
  }
  ignore_this_mmap = true;
  assert(nvmfd >= 0);
  ignore_this_mmap = false;

  uffd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
  if (uffd == -1) {
    perror("uffd");
    ignore_this_mmap = true;
    assert(0);
    ignore_this_mmap = false;
  }

  uffdio_api.api = UFFD_API;
  uffdio_api.features = UFFD_FEATURE_PAGEFAULT_FLAG_WP |  UFFD_FEATURE_MISSING_SHMEM | UFFD_FEATURE_MISSING_HUGETLBFS;// | UFFD_FEATURE_EVENT_UNMAP | UFFD_FEATURE_EVENT_REMOVE;
  uffdio_api.ioctls = 0;
  if (ioctl(uffd, UFFDIO_API, &uffdio_api) == -1) {
    perror("ioctl uffdio_api");
    ignore_this_mmap = true;
    assert(0);
    ignore_this_mmap = false;
  }

  int s = pthread_create(&fault_thread, NULL, handle_fault, 0);
  if (s != 0) {
    perror("pthread_create");
    ignore_this_mmap = true;
    assert(0);
    ignore_this_mmap = false;
  }

  devmemfd = open("/dev/mem", O_RDWR | O_SYNC);
  if (devmemfd < 0) {
    perror("/dev/mem open");
    ignore_this_mmap = true;
    assert(0);
    ignore_this_mmap = false;
  }

  timef = fopen("times.txt", "w+");
  if (timef == NULL) {
    perror("time file fopen\n");
    ignore_this_mmap = true;
    assert(0);
    ignore_this_mmap = false;
  }

  dram_devdax_mmap =libc_mmap(NULL, DRAMSIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, dramfd, 0);
  if (dram_devdax_mmap == MAP_FAILED) {
    perror("dram devdax mmap");
    ignore_this_mmap = true;
    assert(0);
    ignore_this_mmap = false;
  }

  nvm_devdax_mmap =libc_mmap(NULL, NVMSIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, nvmfd, 0);
  if (nvm_devdax_mmap == MAP_FAILED) {
    perror("nvm devdax mmap");
    ignore_this_mmap = true;
    assert(0);
    ignore_this_mmap = false;
  }

  uint64_t i;
  int r = pthread_barrier_init(&pmemcpy.barrier, NULL, MAX_COPY_THREADS + 1);
  ignore_this_mmap = true;
  assert(r == 0);
  ignore_this_mmap = false;

  for (i = 0; i < MAX_COPY_THREADS; i++) {
    s = pthread_create(&copy_threads[i], NULL, hemem_parallel_memcpy_thread, (void*)i);
    ignore_this_mmap = true;
    assert(s == 0);
    ignore_this_mmap = false;
  }

  if (ioctl(uffd, UFFDIO_CR3, &uffdio_cr3) < 0) {
    perror("ioctl uffdio_cr3");
    ignore_this_mmap = true;
    assert(0);
    ignore_this_mmap = false;
  }
  cr3 = uffdio_cr3.cr3;

  paging_init();
  
  is_init = true;

  LOG("hemem_init: finished\n");
}

static void hemem_mmap_populate(void* addr, size_t length)
{
  // Page mising fault case - probably the first touch case
  // allocate in DRAM via LRU
  void* newptr;
  uint64_t offset;
  struct hemem_page *page;
  bool in_dram;
  uint64_t page_boundry;
  void* tmpaddr;
  uint64_t pagesize;

  ignore_this_mmap = true;
  assert(addr != 0);
  assert(length != 0);
  ignore_this_mmap = false;

  for (page_boundry = (uint64_t)addr; page_boundry < (uint64_t)addr + length;) {
    page = hemem_get_free_page();
    ignore_this_mmap = true;
    assert(page != NULL);
    ignore_this_mmap = false;
    pagefault(page);

    // let policy algorithm do most of the heavy lifting of finding a free page
    page->prev = page->next = NULL; 
    offset = page->devdax_offset;
    in_dram = page->in_dram;
    pagesize = pt_to_pagesize(page->pt);

    tmpaddr = (in_dram ? dram_devdax_mmap + offset : nvm_devdax_mmap + offset);
    memset(tmpaddr, 0, pagesize);
    memsets++;
  
    // now that we have an offset determined via the policy algorithm, actually map
    // the page for the application
    newptr = libc_mmap((void*)page_boundry, pagesize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE | MAP_FIXED, (in_dram ? dramfd : nvmfd), offset);
    if (newptr == MAP_FAILED) {
      perror("newptr mmap");
      ignore_this_mmap = true;
      assert(0);
      ignore_this_mmap = false;
    }
  
    if (newptr != (void*)page_boundry) {
      fprintf(stderr, "hemem: mmap populate: warning, newptr != page boundry\n");
    }

    // re-register new mmap region with userfaultfd
    struct uffdio_register uffdio_register;
    uffdio_register.range.start = (uint64_t)newptr;
    uffdio_register.range.len = pagesize;
    uffdio_register.mode = UFFDIO_REGISTER_MODE_MISSING | UFFDIO_REGISTER_MODE_WP;
    uffdio_register.ioctls = 0;
    if (ioctl(uffd, UFFDIO_REGISTER, &uffdio_register) == -1) {
      perror("ioctl uffdio_register");
      ignore_this_mmap = true;
      assert(0);
      ignore_this_mmap = false;
    }

    // use mmap return addr to track new page's virtual address
    page->va = (uint64_t)newptr;
    ignore_this_mmap = true;
    assert(page->va != 0);
    ignore_this_mmap = false;
    page->migrating = false;
    page->migrations_up = page->migrations_down = 0;
 
    pthread_mutex_init(&(page->page_lock), NULL);

    mem_allocated += pagesize;

    // place in hemem's page tracking list
    enqueue_page(&list, page);
    page_boundry += pagesize;
  }
}

#define PAGE_ROUND_UP(x) (((x) + (HUGEPAGE_SIZE)-1) & (~((HUGEPAGE_SIZE)-1)))

void* hemem_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
  void *p;
 
  ignore_this_mmap = true;
  assert(is_init);
  ignore_this_mmap = false;

  if ((flags & MAP_PRIVATE) == MAP_PRIVATE) {
    flags &= ~MAP_PRIVATE;
    flags |= MAP_SHARED;
    LOG("hemem_mmap: changed flags to MAP_SHARED\n");
  }

  if ((flags & MAP_ANONYMOUS) == MAP_ANONYMOUS) {
    flags &= ~MAP_ANONYMOUS;
    LOG("hemem_mmap: unset MAP_ANONYMOUS\n");
  }

  if ((flags & MAP_HUGETLB) == MAP_HUGETLB) {
    flags &= ~MAP_HUGETLB;
    LOG("hemem_mmap: unset MAP_HUGETLB\n");
  }
  
  // reserve block of memory
  length = PAGE_ROUND_UP(length);
  p = libc_mmap(addr, length, prot, flags, dramfd, offset);
  if (p == NULL || p == MAP_FAILED) {
    perror("mmap");
  }
  ignore_this_mmap = true;
  assert(p != NULL && p != MAP_FAILED);
  ignore_this_mmap = false;

  // register with uffd
  struct uffdio_register uffdio_register;
  uffdio_register.range.start = (uint64_t)p;
  uffdio_register.range.len = length;
  uffdio_register.mode = UFFDIO_REGISTER_MODE_MISSING | UFFDIO_REGISTER_MODE_WP;
  uffdio_register.ioctls = 0;
  if (ioctl(uffd, UFFDIO_REGISTER, &uffdio_register) == -1) {
    perror("ioctl uffdio_register");
    ignore_this_mmap = true;
    assert(0);
    ignore_this_mmap = false;
  }
  
  //if ((flags & MAP_POPULATE) == MAP_POPULATE) {
    hemem_mmap_populate(p, length);
  //}
  
  return p;
}


int hemem_munmap(void* addr, size_t length)
{
  return libc_munmap(addr, length);
}

static void hemem_parallel_memcpy(void *dst, void *src, size_t length)
{
  /* uint64_t i; */
  /* bool all_threads_done; */

  pmemcpy.dst = dst;
  pmemcpy.src = src;
  pmemcpy.length = length;

  int r = pthread_barrier_wait(&pmemcpy.barrier);
  ignore_this_mmap = true;
  assert(r == 0 || r == PTHREAD_BARRIER_SERIAL_THREAD);
  ignore_this_mmap = false;
  
  LOG("parallel migration started\n");
  
  /* pmemcpy.activate = true; */

  /* while (!all_threads_done) { */
  /*   all_threads_done = true; */
  /*   for (i = 0; i < MAX_COPY_THREADS; i++) { */
  /*     if (!pmemcpy.done_bitmap[i]) { */
  /*       all_threads_done = false; */
  /*       break; */
  /*     } */
  /*   } */
  /* } */

  r = pthread_barrier_wait(&pmemcpy.barrier);
  ignore_this_mmap = true;
  assert(r == 0 || r == PTHREAD_BARRIER_SERIAL_THREAD);
  ignore_this_mmap = false;
  LOG("parallel migration finished\n");

  /* pmemcpy.activate = false; */

  /* for (i = 0; i < MAX_COPY_THREADS; i++) { */
  /*   pmemcpy.done_bitmap[i] = false; */
  /* } */
}

void hemem_migrate_up(struct hemem_page *page, uint64_t dram_offset)
{
  void *old_addr;
  void *new_addr;
  void *newptr;
  struct timeval migrate_start, migrate_end;
  struct timeval start, end;
  uint64_t old_addr_offset, new_addr_offset;
  uint64_t pagesize;

  //LOG("hemem_migrate_up: migrate down addr: %lx pte: %lx\n", page->va, hemem_va_to_pa(page->va));
  
  gettimeofday(&migrate_start, NULL);
  
  ignore_this_mmap = true;
  assert(page != NULL);
  ignore_this_mmap = false;

  pagesize = pt_to_pagesize(page->pt);

  old_addr_offset = page->devdax_offset;
  new_addr_offset = dram_offset;

  old_addr = nvm_devdax_mmap + old_addr_offset;
  ignore_this_mmap = true;
  assert((uint64_t)old_addr_offset < NVMSIZE);
  assert((uint64_t)old_addr_offset + pagesize <= NVMSIZE);
  ignore_this_mmap = false;

  new_addr = dram_devdax_mmap + new_addr_offset;
  ignore_this_mmap = true;
  assert((uint64_t)new_addr_offset < DRAMSIZE);
  assert((uint64_t)new_addr_offset + pagesize <= DRAMSIZE);
  ignore_this_mmap = false;

  // copy page from faulting location to temp location
  gettimeofday(&start, NULL);
  hemem_parallel_memcpy(new_addr, old_addr, pagesize);
  gettimeofday(&end, NULL);
  LOG_TIME("memcpy_to_dram: %f s\n", elapsed(&start, &end));
 
#ifdef HEMEM_DEBUG 
  uint64_t* src = (uint64_t*)old_addr;
  uint64_t* dst = (uint64_t*)new_addr;
  for (int i = 0; i < (pagesize / sizeof(uint64_t)); i++) {
    if (dst[i] != src[i]) {
      LOG("hemem_migrate_up: dst[%d] = %lu != src[%d] = %lu\n", i, dst[i], i, src[i]);
      ignore_this_mmap = true;
      assert(dst[i] == src[i]);
      ignore_this_mmap = false;
    }
  }
#endif

  gettimeofday(&start, NULL);
  ignore_this_mmap = true;
  assert(libc_mmap != NULL);
  ignore_this_mmap = false;
  newptr = libc_mmap((void*)page->va, pagesize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE | MAP_FIXED, dramfd, new_addr_offset);
  if (newptr == MAP_FAILED) {
    perror("newptr mmap");
    ignore_this_mmap = true;
    assert(0);
    ignore_this_mmap = false;
  }
  if (newptr != (void*)page->va) {
    fprintf(stderr, "mapped address is not same as faulting address\n");
  }
  gettimeofday(&end, NULL);
  LOG_TIME("mmap_dram: %f s\n", elapsed(&start, &end));

  // re-register new mmap region with userfaultfd
  gettimeofday(&start, NULL);
  struct uffdio_register uffdio_register;
  uffdio_register.range.start = (uint64_t)newptr;
  uffdio_register.range.len = pagesize;
  uffdio_register.mode = UFFDIO_REGISTER_MODE_MISSING | UFFDIO_REGISTER_MODE_WP;
  uffdio_register.ioctls = 0;
  if (ioctl(uffd, UFFDIO_REGISTER, &uffdio_register) == -1) {
    perror("ioctl uffdio_register");
    ignore_this_mmap = true;
    assert(0);
    ignore_this_mmap = false;
  }
  gettimeofday(&end, NULL);
  LOG_TIME("uffdio_register: %f s\n", elapsed(&start, &end));

  page->migrations_up++;
  migrations_up++;

  page->devdax_offset = dram_offset;
  page->in_dram = true;

  hemem_tlb_shootdown(page->va);
  
  //LOG("hemem_migrate_up: new pte: %lx\n", hemem_va_to_pa(page->va));

  gettimeofday(&migrate_end, NULL);  
  LOG_TIME("hemem_migrate_up: %f s\n", elapsed(&migrate_start, &migrate_end));
}


void hemem_migrate_down(struct hemem_page *page, uint64_t nvm_offset)
{
  void *old_addr;
  void *new_addr;
  void *newptr;
  struct timeval migrate_start, migrate_end;
  struct timeval start, end;
  uint64_t old_addr_offset, new_addr_offset;
  uint64_t pagesize;

  //LOG("hemem_migrate_down: migrate down addr: %lx pte: %lx\n", page->va, hemem_va_to_pa(page->va));

  gettimeofday(&migrate_start, NULL);

  pagesize = pt_to_pagesize(page->pt);
  
  ignore_this_mmap = true;
  assert(page != NULL);
  ignore_this_mmap = false;
  old_addr_offset = page->devdax_offset;
  new_addr_offset = nvm_offset;

  old_addr = dram_devdax_mmap + old_addr_offset;
  ignore_this_mmap = true;
  assert((uint64_t)old_addr_offset < DRAMSIZE);
  assert((uint64_t)old_addr_offset + pagesize <= DRAMSIZE);
  ignore_this_mmap = false;

  new_addr = nvm_devdax_mmap + new_addr_offset;
  ignore_this_mmap = true;
  assert((uint64_t)new_addr_offset < NVMSIZE);
  assert((uint64_t)new_addr_offset + pagesize <= NVMSIZE);
  ignore_this_mmap = false;

  // copy page from faulting location to temp location
  gettimeofday(&start, NULL);
  hemem_parallel_memcpy(new_addr, old_addr, pagesize);
  gettimeofday(&end, NULL);
  LOG_TIME("memcpy_to_nvm: %f s\n", elapsed(&start, &end));

#ifdef HEMEM_DEBUG
  uint64_t* src = (uint64_t*)old_addr;
  uint64_t* dst = (uint64_t*)new_addr;
  for (int i = 0; i < (pagesize / sizeof(uint64_t)); i++) {
    if (dst[i] != src[i]) {
      LOG("hemem_migrate_down: dst[%d] = %lu != src[%d] = %lu\n", i, dst[i], i, src[i]);
      ignore_this_mmap = true;
      assert(dst[i] == src[i]);
      ignore_this_mmap = false;
    }
  }
#endif
  
  gettimeofday(&start, NULL);
  newptr = libc_mmap((void*)page->va, pagesize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE | MAP_FIXED, nvmfd, new_addr_offset);
  if (newptr == MAP_FAILED) {
    perror("newptr mmap");
    ignore_this_mmap = true;
    assert(0);
    ignore_this_mmap = false;
  }
  if (newptr != (void*)page->va) {
    fprintf(stderr, "mapped address is not same as faulting address\n");
  }
  gettimeofday(&end, NULL);
  LOG_TIME("mmap_nvm: %f s\n", elapsed(&start, &end));

  // re-register new mmap region with userfaultfd
  gettimeofday(&start, NULL);
  struct uffdio_register uffdio_register;
  uffdio_register.range.start = (uint64_t)newptr;
  uffdio_register.range.len = pagesize;
  uffdio_register.mode = UFFDIO_REGISTER_MODE_MISSING | UFFDIO_REGISTER_MODE_WP;
  uffdio_register.ioctls = 0;
  if (ioctl(uffd, UFFDIO_REGISTER, &uffdio_register) == -1) {
    perror("ioctl uffdio_register");
    ignore_this_mmap = true;
    assert(0);
    ignore_this_mmap = false;
  }
  gettimeofday(&end, NULL);
  LOG_TIME("uffdio_register: %f s\n", elapsed(&start, &end));
  
  page->migrations_down++;
  migrations_down++;

  page->devdax_offset = nvm_offset;
  page->in_dram = false;

  hemem_tlb_shootdown(page->va);

  //LOG("hemem_migrate_down: new pte: %lx\n", hemem_va_to_pa(page->va));

  gettimeofday(&migrate_end, NULL);  
  LOG_TIME("hemem_migrate_down: %f s\n", elapsed(&migrate_start, &migrate_end));
}

void hemem_wp_page(struct hemem_page *page, bool protect)
{
  uint64_t addr = page->va;
  struct uffdio_writeprotect wp;
  int ret;
  struct timeval start, end;
  uint64_t pagesize = pt_to_pagesize(page->pt);

  //LOG("hemem_wp_page: wp addr %lx pte: %lx\n", addr, hemem_va_to_pa(addr));

  ignore_this_mmap = true;
  assert(addr != 0);
  ignore_this_mmap = false;

  gettimeofday(&start, NULL);
  wp.range.start = addr;
  wp.range.len = pagesize;
  wp.mode = (protect ? UFFDIO_WRITEPROTECT_MODE_WP : 0);
  ret = ioctl(uffd, UFFDIO_WRITEPROTECT, &wp);

  if (ret < 0) {
    perror("uffdio writeprotect");
    ignore_this_mmap = true;
    assert(0);
    ignore_this_mmap = false;
  }
  gettimeofday(&end, NULL);

  hemem_tlb_shootdown(page->va);
 
  LOG_TIME("uffdio_writeprotect: %f s\n", elapsed(&start, &end));
}


void handle_wp_fault(uint64_t page_boundry)
{
  struct hemem_page *page;

  page = find_page(&list, page_boundry);
  ignore_this_mmap = true;
  assert(page != NULL);
  ignore_this_mmap = false;

  LOG("hemem: handle_wp_fault: waiting for migration for page %lx\n", page_boundry);

  pthread_mutex_lock(&(page->page_lock));

  ignore_this_mmap = true;
  assert(!page->migrating);
  ignore_this_mmap = false;

  hemem_tlb_shootdown(page->va);

  pthread_mutex_unlock(&(page->page_lock));
}


void handle_missing_fault(uint64_t page_boundry)
{
  // Page mising fault case - probably the first touch case
  // allocate in DRAM via LRU
  void* newptr;
  struct timeval missing_start, missing_end;
  struct timeval start, end;
  struct hemem_page *page;
  uint64_t offset;
  void* tmp_offset;
  bool in_dram;
  uint64_t pagesize;

  ignore_this_mmap = true;
  assert(page_boundry != 0);
  ignore_this_mmap = false;

  // have we seen this page before?
  page = find_page(&list, page_boundry);
  if (page != NULL) {
    // if yes, must have unmapped it for migration, wait for migration to finish
    LOG("hemem: encountered a page in the middle of migration, waiting\n");
    handle_wp_fault(page_boundry);
    return;
  }

  gettimeofday(&missing_start, NULL);

  page = hemem_get_free_page();
  ignore_this_mmap = true;
  assert(page != NULL);
  ignore_this_mmap = false;
  
  gettimeofday(&start, NULL);
  // let policy algorithm do most of the heavy lifting of finding a free page
  pagefault(page); 
  gettimeofday(&end, NULL);
  page->prev = page->next = NULL;
  LOG_TIME("page_fault: %f s\n", elapsed(&start, &end));
  offset = page->devdax_offset;
  in_dram = page->in_dram;
  pagesize = pt_to_pagesize(page->pt);

  tmp_offset = (in_dram) ? dram_devdax_mmap + offset : nvm_devdax_mmap + offset;

  memset(tmp_offset, 0, pagesize);
  memsets++;

#ifdef HEMEM_DEBUG
  char* tmp = (char*)tmp_offset;
  for (int i = 0; i < pagesize; i++) {
    ignore_this_mmap = true;
    assert(tmp[i] == 0);
    ignore_this_mmap = false;
  }
#endif

  // now that we have an offset determined via the policy algorithm, actually map
  // the page for the application
  gettimeofday(&start, NULL);
  newptr = libc_mmap((void*)page_boundry, pagesize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE | MAP_FIXED, (in_dram ? dramfd : nvmfd), offset);
  if (newptr == MAP_FAILED) {
    perror("newptr mmap");
    /* free(page); */
    ignore_this_mmap = true;
    assert(0);
    ignore_this_mmap = false;
  }
  LOG("hemem: mmaping at %p\n", newptr);
  if(newptr != (void *)page_boundry) {
    fprintf(stderr, "Not mapped where expected (%p != %p)\n", newptr, (void *)page_boundry);
    ignore_this_mmap = true;
    assert(0);
    ignore_this_mmap = false;
  }
  gettimeofday(&end, NULL);
  LOG_TIME("mmap_%s: %f s\n", (in_dram ? "dram" : "nvm"), elapsed(&start, &end));

  gettimeofday(&start, NULL);
  // re-register new mmap region with userfaultfd
  struct uffdio_register uffdio_register;
  uffdio_register.range.start = (uint64_t)newptr;
  uffdio_register.range.len = pagesize;
  uffdio_register.mode = UFFDIO_REGISTER_MODE_MISSING | UFFDIO_REGISTER_MODE_WP;
  uffdio_register.ioctls = 0;
  if (ioctl(uffd, UFFDIO_REGISTER, &uffdio_register) == -1) {
    perror("ioctl uffdio_register");
    ignore_this_mmap = true;
    assert(0);
    ignore_this_mmap = false;
  }
  gettimeofday(&end, NULL);
  LOG_TIME("uffdio_register: %f s\n", elapsed(&start, &end));

  // use mmap return addr to track new page's virtual address
  page->va = (uint64_t)newptr;
  ignore_this_mmap = true;
  assert(page->va != 0);
  ignore_this_mmap= false;
  page->migrating = false;
  page->migrations_up = page->migrations_down = 0;
 
  pthread_mutex_init(&(page->page_lock), NULL);

  mem_allocated += pagesize;

  //LOG("hemem_missing_fault: va: %lx assigned to %s frame %lu  pte: %lx\n", page->va, (in_dram ? "DRAM" : "NVM"), page->devdax_offset / pagesize, hemem_va_to_pa(page->va));

  // place in hemem's page tracking list
  enqueue_page(&list, page);

  missing_faults_handled++;
  gettimeofday(&missing_end, NULL);
  LOG_TIME("hemem_missing_fault: %f s\n", elapsed(&missing_start, &missing_end));
}



void *handle_fault()
{
  static struct uffd_msg msg[MAX_UFFD_MSGS];
  ssize_t nread;
  uint64_t fault_addr;
  uint64_t fault_flags;
  uint64_t page_boundry;
  struct uffdio_range range;
  int ret;
  int nmsgs;
  int i;

  for (;;) {
    struct pollfd pollfd;
    int pollres;
    pollfd.fd = uffd;
    pollfd.events = POLLIN;

    pollres = poll(&pollfd, 1, -1);

    switch (pollres) {
    case -1:
      perror("poll");
      ignore_this_mmap = true;
      assert(0);
      ignore_this_mmap = false;
    case 0:
      fprintf(stderr, "poll read 0\n");
      continue;
    case 1:
      break;
    default:
      fprintf(stderr, "unexpected poll result\n");
      ignore_this_mmap = true;
      assert(0);
      ignore_this_mmap = false;
    }

    if (pollfd.revents & POLLERR) {
      fprintf(stderr, "pollerr\n");
      ignore_this_mmap = true;
      assert(0);
      ignore_this_mmap = false;
    }

    if (!pollfd.revents & POLLIN) {
      continue;
    }

    nread = read(uffd, &msg[0], MAX_UFFD_MSGS * sizeof(struct uffd_msg));
    if (nread == 0) {
      fprintf(stderr, "EOF on userfaultfd\n");
      ignore_this_mmap = true;
      assert(0);
      ignore_this_mmap = false;
    }

    if (nread < 0) {
      if (errno == EAGAIN) {
        continue;
      }
      perror("read");
      ignore_this_mmap = true;
      assert(0);
      ignore_this_mmap = false;
    }

    if ((nread % sizeof(struct uffd_msg)) != 0) {
      fprintf(stderr, "invalid msg size: [%ld]\n", nread);
      ignore_this_mmap = true;
      assert(0);
      ignore_this_mmap = false;
    }

    nmsgs = nread / sizeof(struct uffd_msg);

    for (i = 0; i < nmsgs; i++) {
      //TODO: check page fault event, handle it
      if (msg[i].event & UFFD_EVENT_PAGEFAULT) {
        fault_addr = (uint64_t)msg[i].arg.pagefault.address;
        fault_flags = msg[i].arg.pagefault.flags;

        // allign faulting address to page boundry
        // huge page boundry in this case due to dax allignment
        page_boundry = fault_addr & ~(PAGE_SIZE - 1);

        if (fault_flags & UFFD_PAGEFAULT_FLAG_WP) {
          handle_wp_fault(page_boundry);
        }
        else {
          handle_missing_fault(page_boundry);
        }

        // wake the faulting thread
        range.start = (uint64_t)page_boundry;
        range.len = PAGE_SIZE;

        ret = ioctl(uffd, UFFDIO_WAKE, &range);

        if (ret < 0) {
          perror("uffdio wake");
          ignore_this_mmap = true;
          assert(0);
          ignore_this_mmap = false;
        }
      }
      else if (msg[i].event & UFFD_EVENT_UNMAP){
        fprintf(stderr, "Received an unmap event\n");
        ignore_this_mmap = true;
        assert(0);
        ignore_this_mmap = false;
      }
      else if (msg[i].event & UFFD_EVENT_REMOVE) {
        fprintf(stderr, "received a remove event\n");
        ignore_this_mmap = true;
        assert(0);
        ignore_this_mmap = false;
      }
      else {
        fprintf(stderr, "received a non page fault event\n");
        ignore_this_mmap = true;
        assert(0);
        ignore_this_mmap = false;
      }
    }
  }
}


uint64_t hemem_va_to_pa(uint64_t va)
{
  uint64_t page_boundry = va & ~(PAGE_SIZE - 1);
  return va_to_pa(page_boundry);
}


void hemem_tlb_shootdown(uint64_t va)
{
  uint64_t page_boundry = va & ~(PAGE_SIZE - 1);
  struct uffdio_range range;
  int ret;

  range.start = page_boundry;
  range.len = PAGE_SIZE;

  ret = ioctl(uffd, UFFDIO_TLBFLUSH, &range);
  if (ret < 0) {
    perror("uffdio tlbflush");
    assert(0);
  }
}


void hemem_clear_accessed_bit(uint64_t va)
{
  uint64_t page_boundry = va & ~(PAGE_SIZE - 1);
  struct uffdio_range range;
  int ret;

  clear_accessed_bit(page_boundry);
 
  range.start = page_boundry;
  range.len = PAGE_SIZE;

  ret = ioctl(uffd, UFFDIO_TLBFLUSH, &range);
  if (ret < 0) {
    perror("uffdio tlbflush");
    assert(0);
  }
}


int hemem_get_accessed_bit(uint64_t va)
{
  uint64_t page_boundry = va & ~(PAGE_SIZE - 1);

  return get_accessed_bit(page_boundry);
}


void hemem_print_stats()
{
  fprintf(stderr, "missing_faults_handled: [%lu]\tmigrations_up: [%lu]\tmigrations_down: [%lu]\tpmemcpys: [%lu]\tmemsets: [%lu]\n", missing_faults_handled, migrations_up, migrations_down, pmemcpys, memsets);
}


void hemem_clear_stats()
{
  missing_faults_handled = 0;
  migrations_up = 0;
  migrations_down = 0;
  pmemcpys = 0;
  memsets = 0;
}
