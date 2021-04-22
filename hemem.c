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
#include "lru.h"
#include "coalesce.h"
#include "aligned.h"
#include "bitmap.h"
#include "uthash.h"
#include "pebs.h"

pthread_t fault_thread;

int dramfd = -1;
int nvmfd = -1;
int devmemfd = -1;
long uffd = -1;

bool is_init = false;
bool timing = false;

uint64_t mem_mmaped = 0;
uint64_t mem_allocated = 0;
uint64_t pages_allocated = 0;
uint64_t pages_freed = 0;
uint64_t fastmem_allocated = 0;
uint64_t slowmem_allocated = 0;
uint64_t wp_faults_handled = 0;
uint64_t missing_faults_handled = 0;
uint64_t migrations_up = 0;
uint64_t migrations_down = 0;
uint64_t bytes_migrated = 0;
uint64_t pmemcpys = 0;
uint64_t memsets = 0;
uint64_t migration_waits = 0;

static bool cr3_set = false;
uint64_t cr3 = 0;

pthread_t copy_threads[MAX_COPY_THREADS];
pthread_t stats_thread;

//#define MAXPAGES	262144
//#define MAXPAGES	10000000
//static struct hemem_page freepages[MAXPAGES];
//static size_t nextfreepage = 0;
struct hemem_page *pages = NULL;
pthread_mutex_t pages_lock = PTHREAD_MUTEX_INITIALIZER;

void *dram_devdax_mmap;
void *nvm_devdax_mmap;
//void *devmem_mmap;

__thread bool internal_call = false;
__thread bool old_internal_call = false;

struct pmemcpy {
  pthread_mutex_t lock;
  pthread_barrier_t barrier;
  _Atomic bool write_zeros;
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

  assert(tid < MAX_COPY_THREADS);
  

  for (;;) {
    /* while(!pmemcpy.activate || pmemcpy.done_bitmap[tid]) { } */
    int r = pthread_barrier_wait(&pmemcpy.barrier);
    assert(r == 0 || r == PTHREAD_BARRIER_SERIAL_THREAD);
    if (tid == 0) {
      pmemcpys++;
    }

    // grab data out of shared struct
    length = pmemcpy.length;
    chunk_size = length / MAX_COPY_THREADS;
    dst = pmemcpy.dst + (tid * chunk_size);
    if (!pmemcpy.write_zeros) {
      src = pmemcpy.src + (tid * chunk_size);
      memcpy(dst, src, chunk_size);
    }
    else {
      memset(dst, 0, chunk_size);
    }

#ifdef HEMEM_DEBUG
    uint64_t *tmp1, *tmp2, i;
    tmp1 = dst;
    tmp2 = src;
    for (i = 0; i < chunk_size / sizeof(uint64_t); i++) {
      if (tmp1[i] != tmp2[i]) {
        LOG("copy thread: dst[%lu] = %lu != src[%lu] = %lu\n", i, tmp1[i], i, tmp2[i]);
        assert(tmp1[i] == tmp2[i]);
      }
    }
#endif

    //LOG("thread %lu done copying\n", tid);

    r = pthread_barrier_wait(&pmemcpy.barrier);
    assert(r == 0 || r == PTHREAD_BARRIER_SERIAL_THREAD);
    /* pmemcpy.done_bitmap[tid] = true; */
  }
  return NULL;
}


static void *hemem_stats_thread()
{
  for (;;) {
    sleep(1);
    
    hemem_print_stats();
    hemem_clear_stats();
  }
  return NULL;
}

void enqueue_fifo(struct fifo_list *queue, struct hemem_page *entry)
{
  //assert(page->prev == NULL);
  //page->next = list.first;
  //if (list.first != NULL) {
    //assert(list.first->prev == NULL);
    //list.first->prev = page;
  
  pthread_mutex_lock(&(queue->list_lock));
  assert(entry->prev == NULL);
  entry->next = queue->first;
  if(queue->first != NULL) {
    assert(queue->first->prev == NULL);
    queue->first->prev = entry;
  } else {
    assert(queue->last == NULL);
    assert(queue->numentries == 0);
    queue->last = entry;
  }

  queue->first = entry;
  queue->numentries++;
  pthread_mutex_unlock(&(queue->list_lock));
}

struct hemem_page *dequeue_fifo(struct fifo_list *queue)
{
  pthread_mutex_lock(&(queue->list_lock));
  struct hemem_page *ret = queue->last;

  if(ret == NULL) {
    assert(queue->numentries == 0);
    pthread_mutex_unlock(&(queue->list_lock));
    return ret;
  }

  queue->last = ret->prev;
  if(queue->last != NULL) {
    queue->last->next = NULL;
  } else {
    queue->first = NULL;
  }

  ret->prev = ret->next = NULL;
  assert(queue->numentries > 0);
  queue->numentries--;
  pthread_mutex_unlock(&(queue->list_lock));

  mem_allocated -= ret->size;
  queue->numentries--;
  
  return ret;
}

void add_page(struct hemem_page *page)
{
  struct hemem_page *p;
  pthread_mutex_lock(&pages_lock);
  HASH_FIND(hh, pages, &(page->va), sizeof(uint64_t), p);
  assert(p == NULL);
  HASH_ADD(hh, pages, va, sizeof(uint64_t), page);
  pthread_mutex_unlock(&pages_lock);
}

void remove_page(struct hemem_page *page)
{
  pthread_mutex_lock(&pages_lock);
  HASH_DEL(pages, page);
  pthread_mutex_unlock(&pages_lock);
}

struct hemem_page* find_page(uint64_t va)
{
  struct hemem_page *page;
  HASH_FIND(hh, pages, &va, sizeof(uint64_t), page);
  return page;
}

void print_pages()
{
  //struct hemem_page *cur = list.first;

  //while (cur != NULL) {
  //  printf("page addr %p\n", cur->va);
  //  cur = cur->next;
  //}

  //return NULL;
}


int delete_page(uint64_t va)
{
  /*struct hemem_page *cur = list.first;
  struct hemem_page *next;

  if(list.numentries == 1){
    mem_allocated -= cur->size;
    list.first = NULL;
    list.last = NULL;
    list.numentries--;
    return 1;
  }
  if(cur->va == va){
    mem_allocated -= cur->size;
    list.first = cur->next;
    list.numentries--;
    //free(cur);
    return 1;
  }
  while (cur->next != NULL) {
    if (cur->next->va == va) {
      next = cur->next;
      cur->next = next->next;
      mem_allocated -= next->size;
      list.numentries--;
      //free(next);
      return 1;
    }
    cur = cur->next;
  }
  return 0;*/
  struct hemem_page* page;
  HASH_FIND(hh, pages, &va, sizeof(uint64_t), page);
  if(page) {
	  mem_allocated -= page->size;
	  HASH_DEL(pages, page);
	  return 1;
  }
  return 0;
}



void hemem_init()
{
  struct uffdio_api uffdio_api;

  internal_call = true;
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
    assert(0);
  }

  LOG("hemem_init: started\n");

  dramfd = open(DRAMPATH, O_RDWR);
  if (dramfd < 0) {
    perror("dram open");
  }
  assert(dramfd >= 0);

  nvmfd = open(NVMPATH, O_RDWR);
  if (nvmfd < 0) {
    perror("nvm open");
  }
  assert(nvmfd >= 0);

  devmemfd = open("/dev/mem", O_RDWR | O_SYNC);
  if (devmemfd < 0) {
    perror("devmem open");
    assert(0);
  }

  uffd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
  if (uffd == -1) {
    perror("uffd");
    assert(0);
  }

  uffdio_api.api = UFFD_API;
  uffdio_api.features = UFFD_FEATURE_PAGEFAULT_FLAG_WP |  UFFD_FEATURE_MISSING_SHMEM | UFFD_FEATURE_MISSING_HUGETLBFS;// | UFFD_FEATURE_EVENT_UNMAP | UFFD_FEATURE_EVENT_REMOVE;
  uffdio_api.ioctls = 0;
  if (ioctl(uffd, UFFDIO_API, &uffdio_api) == -1) {
    perror("ioctl uffdio_api");
    assert(0);
  }

  int s = pthread_create(&fault_thread, NULL, handle_fault, 0);
  if (s != 0) {
    perror("pthread_create");
    assert(0);
  }

  timef = fopen("times.txt", "w+");
  if (timef == NULL) {
    perror("time file fopen\n");
    assert(0);
  }

  statsf = fopen("stats.txt", "w+");
  if (statsf == NULL) {
    perror("stats file fopen\n");
    assert(0);
  }

#if DRAMSIZE != 0
  dram_devdax_mmap =libc_mmap(NULL, DRAMSIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, dramfd, 0);
  if (dram_devdax_mmap == MAP_FAILED) {
    perror("dram devdax mmap");
    assert(0);
  }
#endif

  nvm_devdax_mmap =libc_mmap(NULL, NVMSIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, nvmfd, 0);
  if (nvm_devdax_mmap == MAP_FAILED) {
    perror("nvm devdax mmap");
    assert(0);
  }

  //devmem_mmap = libc_mmap(NULL, 6762176548864, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, devmemfd, 0);
  //if (devmem_mmap == MAP_FAILED) {
    //perror("devmem mmap");
    //assert(0);
  //}
 
  uint64_t i;
  int r = pthread_barrier_init(&pmemcpy.barrier, NULL, MAX_COPY_THREADS + 1);
  assert(r == 0);

  r = pthread_mutex_init(&pmemcpy.lock, NULL);
  assert(r == 0);

  for (i = 0; i < MAX_COPY_THREADS; i++) {
    s = pthread_create(&copy_threads[i], NULL, hemem_parallel_memcpy_thread, (void*)i);
    assert(s == 0);
  }

#ifdef STATS_THREAD
  s = pthread_create(&stats_thread, NULL, hemem_stats_thread, NULL);
  assert(s == 0);
#endif

  paging_init();
#ifdef COALESCE
  coalesce_init();
#endif

#ifdef USE_PEBS
  pebs_init();
#endif
  
  is_init = true;

  struct hemem_page *dummy_page = calloc(1, sizeof(struct hemem_page));
  add_page(dummy_page);

  LOG("hemem_init: finished\n");

  internal_call = false;
}

static void hemem_parallel_memset(void* addr, int c, size_t n)
{
  pthread_mutex_lock(&(pmemcpy.lock));
  pmemcpy.dst = addr;
  pmemcpy.length = n;
  pmemcpy.write_zeros = true;

  int r = pthread_barrier_wait(&pmemcpy.barrier);
  assert(r ==0 || r == PTHREAD_BARRIER_SERIAL_THREAD);

  r = pthread_barrier_wait(&pmemcpy.barrier);
  assert(r == 0 || r == PTHREAD_BARRIER_SERIAL_THREAD);
  
  pthread_mutex_unlock(&(pmemcpy.lock));
}

static void hemem_mmap_populate(void* addr, size_t length)
{
  void* p;
  struct uffdio_cr3 uffdio_base;
  // Page mising fault case - probably the first touch case
  // allocate in DRAM via LRU
  void* newptr;
  uint64_t offset;
  struct hemem_page *page;
  bool in_dram;
  uint64_t page_boundry;
  void* tmpaddr;
  uint64_t pagesize;

  assert(addr != 0);
  assert(length != 0);

  policy_lock();

  LOG("hemem_mmap_populate: addr: 0x%lx, npages: %lu\n", (uint64_t)addr, npages);
//  for (i = 0; i < npages; i++) {
//    printf("populating page %lu out of %lu\n", i , npages);
//    internal_malloc = true;
//    page = (struct hemem_page*)calloc(1, sizeof(struct hemem_page));
//    internal_malloc = false;
  //  if (page == NULL) {
    //  perror("page calloc");
    //  assert(0);
   // }
  for (page_boundry = (uint64_t)addr; page_boundry < (uint64_t)addr + length;) {
    //page = pagefault_unlocked();
  
#ifdef COALESCE
    void* huge_page = check_aligned(page_boundry);

    if(huge_page) {
      aligned_pagefault(page, huge_page);
      check_in_dram(page, huge_page, dramfd, nvmfd);
    } else {
      page = pagefault_unlocked();
      //pagefault(page); 
    }
#else
    page = pagefault_unlocked();
    //pagefault(page);
#endif
    assert(page != NULL);

    page->va = page_boundry; 
    // let policy algorithm do most of the heavy lifting of finding a free page
    offset = page->devdax_offset;
    in_dram = page->in_dram;
    pagesize = pt_to_pagesize(page->pt);

    tmpaddr = (in_dram ? dram_devdax_mmap + offset : nvm_devdax_mmap + offset);
    hemem_parallel_memset(tmpaddr, 0, pagesize);
    memsets++;
  
    // now that we have an offset determined via the policy algorithm, actually map
    // the page for the application
    newptr = libc_mmap((void*)page_boundry, pagesize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE | MAP_FIXED, (in_dram ? dramfd : nvmfd), offset);
    if (newptr == MAP_FAILED) {
      perror("newptr mmap");
      assert(0);
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
      assert(0);
    }

    // use mmap return addr to track new page's virtual address
    page->va = (uint64_t)newptr;
    assert(page->va != 0);
    assert(page->va % HUGEPAGE_SIZE == 0);
    page->migrating = false;
    page->migrations_up = page->migrations_down = 0;
    //page->pa = hemem_va_to_pa(page);
 
#ifdef COALESCE 
    LOG("in populate: incrementing huge page %p with fd %u or %u\n", page->va, dramfd, nvmfd);
    if(page->in_dram) incr_dram_huge_page(page->va, dramfd, offset);
    else incr_nvm_huge_page(page->va, nvmfd, offset);
#endif
   
    pthread_mutex_init(&(page->page_lock), NULL);

    //mem_allocated += PAGE_SIZE;
 
    mem_allocated += pagesize;
    pages_allocated++;

    // place in hemem's page tracking list
    add_page(page);
    page_boundry += pagesize;
  }

  policy_unlock();
}

#define PAGE_ROUND_UP(x) (((x) + (HUGEPAGE_SIZE)-1) & (~((HUGEPAGE_SIZE)-1)))

void* hemem_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
  void *p;
  struct uffdio_cr3 uffdio_cr3;

  //LOG("trying mmap\n");

  internal_call = true;

  assert(is_init);
  assert(length != 0);
  
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
  assert(p != NULL && p != MAP_FAILED);

  LOG("mmaped at %p with offset %p\n", p, offset);
  // register with uffd
  struct uffdio_register uffdio_register;
  uffdio_register.range.start = (uint64_t)p;
  uffdio_register.range.len = length;
  uffdio_register.mode = UFFDIO_REGISTER_MODE_MISSING | UFFDIO_REGISTER_MODE_WP;
  uffdio_register.ioctls = 0;
  if (ioctl(uffd, UFFDIO_REGISTER, &uffdio_register) == -1) {
    perror("ioctl uffdio_register");
    assert(0);
  }

  if (!cr3_set) {
    if (ioctl(uffd, UFFDIO_CR3, &uffdio_cr3) < 0) {
      perror("ioctl uffdio_cr3");
      assert(0);
    }
    cr3 = uffdio_cr3.cr3;
    cr3_set = true;
  }

   
  if ((flags & MAP_POPULATE) == MAP_POPULATE) {
    hemem_mmap_populate(p, length);
  }

  mem_mmaped = length;
  
  internal_call = false;
  
  return p;
}

int hemem_madvise(void* addr, size_t length, int advice){

  uint64_t i;
  uint64_t hp_bound = ((uint64_t) addr) & (~HUGEPAGE_MASK);
  void* huge_page = check_aligned(hp_bound);
  struct hemem_page check;

  LOG2("madvise dont need\n");
  
  if(huge_page) {
    check_in_dram(&check, huge_page, dramfd, nvmfd);
    //struct page* this_hp = find_page(hp_bound);

    for(i = (uint64_t) addr; i < (uint64_t) (addr+length); i+= BASEPAGE_SIZE){
      
      if(check.in_dram) decr_dram_huge_page(i);
      else decr_nvm_huge_page(i);
      
      struct hemem_page* this_page = find_page(i);
      if(this_page && this_page->size == BASEPAGE_SIZE){ 
        //LOG2("deleting base page %p from list\n", i);
        //remove_page(this_page);
        delete_page(i);
      }
    }
  } else {
    LOG2("freeing page from nonexistant huge page, shouldn't happen\n");
    exit(0);
  }
  
  return 0;
}

void hemem_combine_base_pages(uint64_t addr){

  uint64_t i = addr;
  struct hemem_page* page, *old_page;
  void* ret;
  int fd, ret2;

  //assert(nextfreepage < MAXPAGES);
  //page = &freepages[nextfreepage++];

  do{
    old_page = find_page(i);
    i+=BASEPAGE_SIZE;
  }
  while(old_page == NULL && i<(addr + HUGEPAGE_SIZE));

  if(i >= (addr + HUGEPAGE_SIZE)){
    printf("can't find base pages to coalesce, this shouldn't happen\n");
    printf("addr = %p, i = %p\n", addr, i);
    print_pages();
    //LOG("can't find base pages to coalesce, this shouldn't happen\n");
    exit(0);
    return;
  }

  page = old_page;

  if(old_page->in_dram) fd = dramfd;
  else fd = nvmfd;

  page->va = addr;
  page->devdax_offset = old_page->devdax_offset & (~HUGEPAGE_MASK);
  page->in_dram = old_page->in_dram;
  page->migrating = old_page->migrating;
  page->migrations_up = page->migrations_down = 0;
  page->size = HUGEPAGE_SIZE;
  page->pt = pagesize_to_pt(PAGE_SIZE);
  mem_allocated += HUGEPAGE_SIZE;

  LOG("created huge page at %p with offset %p\n", page->va, page->devdax_offset);
  //printf("check 1, %p up to %p\n",addr,addr+HUGEPAGE_SIZE);
  for(i = (uint64_t) addr; i < (uint64_t) (addr+HUGEPAGE_SIZE); i+= BASEPAGE_SIZE){
    struct hemem_page* this_page = find_page(i);
    if(this_page){
      //if(this_page->in_dram) decr_dram_huge_page(this_page->va);
      //else decr_nvm_huge_page(this_page->va);
      //delete_page(this_page->va);
 
      //LOG2("unmapping base page at %p with offset %p\n", i, page->devdax_offset + (i-addr);
      //ret2 = libc_munmap((void*) i, BASEPAGE_SIZE);
      //if(ret2 < 0) perror("coalesce munmap\n");
    
      //printf("page %p found\n", i);
      //remove_page(i);
    } else {      
      LOG("filling base page at %p with offset %p\n", i, page->devdax_offset + (i-addr));
      //ret = libc_mmap((void*) i, BASEPAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE | MAP_FIXED, fd, page->devdax_offset + (i-addr));
      //if(ret == MAP_FAILED) perror("coalesce mmap\n");
      handle_missing_fault(i);
      //delete_page(i);
    }
    //else printf("page %p not found\n", i);
    delete_page(i);
  }
    
  enqueue_page(page);
  LOG("mapping huge page at %p with offset %p\n", addr, page->devdax_offset);
  ret = libc_mmap((void*) addr, HUGEPAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE | MAP_FIXED, fd, page->devdax_offset);
  if(ret == MAP_FAILED) perror("coalesce mmap\n");

  assert(ret == addr);

  struct uffdio_register uffdio_register;
  uffdio_register.range.start = (uint64_t)addr;
  uffdio_register.range.len = HUGEPAGE_SIZE;
  uffdio_register.mode = UFFDIO_REGISTER_MODE_MISSING | UFFDIO_REGISTER_MODE_WP;
  uffdio_register.ioctls = 0;
  if (ioctl(uffd, UFFDIO_REGISTER, &uffdio_register) == -1) {
    perror("ioctl uffdio_register");
    assert(0);
  }
  
  hemem_huge_tlb_shootdown(addr);
  //printf("huge page combined\n");
}

void hemem_break_huge_page(uint64_t addr, uint32_t fd, uint64_t offset, struct bitmap* map){

  int i;
  LOG2("breaking huge page at %p\n", addr);
  struct hemem_page* old_page = find_page(addr);
  struct hemem_page* page;
  uint64_t offset;

  //remove_page(old_page);
  delete_page(addr);

  if(old_page == NULL){ 
    LOG2("big problem\n"); 
    print_pages();
  }

  //if(libc_munmap(addr, HUGEPAGE_SIZE) < 0) perror("munmap");
  
  for(i = 0; i < (NUM_SMPAGES-1); i++){
    
    page = dequeue_fifo(&(old_page->base_page_list));

    offset = (page->va - old_page->va) / BASEPAGE_SIZE;

    if(bitmap_get(map, offset)){
      //LOG2("found small page at %d\n", i);
      //page = &freepages[nextfreepage++];
      //printf("getting page for breakdown (shouldn't happen)\n");
      //page->va = addr + (i*BASEPAGE_SIZE);
      //page->devdax_offset = old_page->devdax_offset + i*BASEPAGE_SIZE;
      //page->in_dram = old_page->in_dram;
      //page->migrating = old_page->migrating;
      //page->migrations_up = page->migrations_down = 0;
      //page->size = BASEPAGE_SIZE;
      //mem_allocated += BASEPAGE_SIZE;
      enqueue_page(page);

      libc_mmap((void*) (addr + BASEPAGE_SIZE*i), BASEPAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, offset + BASEPAGE_SIZE*i);
    }
    else {

    }
  }
}

int hemem_munmap(void* addr, size_t length)
{
  uint64_t page_boundry;
  struct hemem_page *page;
  int ret;

  internal_call = true;


  //fprintf(stderr, "munmap(%p, %lu)\n", addr, length);
#ifdef USE_PEBS
  //pebs_print();
  //pebs_clear();
#endif

  //policy_lock();

  // for each page in region specified...
  for (page_boundry = (uint64_t)addr; page_boundry < (uint64_t)addr + length;) {
    // find the page in hemem's trackign list
    page = find_page(page_boundry);
    if (page != NULL) {
      // remove page form hemem's and policy's list
      remove_page(page);
      mmgr_remove(page);

      mem_allocated -= pt_to_pagesize(page->pt);
      mem_mmaped -= pt_to_pagesize(page->pt);
      pages_freed++;

      // move to next page
      page_boundry += pt_to_pagesize(page->pt);
    }
    else {
      // TODO: deal with holes?
      //LOG("hemem_mmunmap: no page to umnap\n");
      //assert(0);
      page_boundry += BASEPAGE_SIZE;
    }
  }

  //policy_unlock();

  ret = libc_munmap(addr, length);

  internal_call = false;

  return ret;
}

static void hemem_parallel_memcpy(void *dst, void *src, size_t length)
{
  /* uint64_t i; */
  /* bool all_threads_done; */
  pthread_mutex_lock(&(pmemcpy.lock));
  pmemcpy.dst = dst;
  pmemcpy.src = src;
  pmemcpy.length = length;
  pmemcpy.write_zeros = false;

  int r = pthread_barrier_wait(&pmemcpy.barrier);
  assert(r == 0 || r == PTHREAD_BARRIER_SERIAL_THREAD);
  
  //LOG("parallel migration started\n");
  
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
  assert(r == 0 || r == PTHREAD_BARRIER_SERIAL_THREAD);
  //LOG("parallel migration finished\n");
  pthread_mutex_unlock(&(pmemcpy.lock));

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

  internal_call = true;

  assert(!page->in_dram);

  //LOG("hemem_migrate_up: migrate down addr: %lx pte: %lx\n", page->va, hemem_va_to_pa(page->va));
  
  gettimeofday(&migrate_start, NULL);
  
  assert(page != NULL);

  pagesize = pt_to_pagesize(page->pt);

  old_addr_offset = page->devdax_offset;
  new_addr_offset = dram_offset;

  old_addr = nvm_devdax_mmap + old_addr_offset;
  assert((uint64_t)old_addr_offset < NVMSIZE);
  assert((uint64_t)old_addr_offset + pagesize <= NVMSIZE);

  new_addr = dram_devdax_mmap + new_addr_offset;
  assert((uint64_t)new_addr_offset < DRAMSIZE);
  assert((uint64_t)new_addr_offset + pagesize <= DRAMSIZE);

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
      assert(dst[i] == src[i]);
    }
  }
#endif

  gettimeofday(&start, NULL);
  assert(libc_mmap != NULL);
  newptr = libc_mmap((void*)page->va, pagesize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE | MAP_FIXED, dramfd, new_addr_offset);
  if (newptr == MAP_FAILED) {
    perror("newptr mmap");
    assert(0);
  }
  if (newptr != (void*)page->va) {
    fprintf(stderr, "mapped address is not same as faulting address\n");
  }
  assert(page->va % HUGEPAGE_SIZE == 0);
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
    assert(0);
  }
  gettimeofday(&end, NULL);
  LOG_TIME("uffdio_register: %f s\n", elapsed(&start, &end));

  page->migrations_up++;
  migrations_up++;

  page->devdax_offset = dram_offset;
  page->in_dram = true;
  //page->pa = hemem_va_to_pa(page);

#ifdef COALESCE
  migrate_to_dram_hp((uint64_t) newptr, dramfd, new_addr_offset);
#endif
  hemem_tlb_shootdown(page->va);

  bytes_migrated += pagesize;
  
  //LOG("hemem_migrate_up: new pte: %lx\n", hemem_va_to_pa(page->va));

  gettimeofday(&migrate_end, NULL);  
  LOG_TIME("hemem_migrate_up: %f s\n", elapsed(&migrate_start, &migrate_end));

  internal_call = false;
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

  internal_call = true;

  assert(page->in_dram);

  //LOG("hemem_migrate_down: migrate down addr: %lx pte: %lx\n", page->va, hemem_va_to_pa(page->va));

  gettimeofday(&migrate_start, NULL);

  pagesize = pt_to_pagesize(page->pt);
  
  assert(page != NULL);
  old_addr_offset = page->devdax_offset;
  new_addr_offset = nvm_offset;

  old_addr = dram_devdax_mmap + old_addr_offset;
  assert((uint64_t)old_addr_offset < DRAMSIZE);
  assert((uint64_t)old_addr_offset + pagesize <= DRAMSIZE);

  new_addr = nvm_devdax_mmap + new_addr_offset;
  assert((uint64_t)new_addr_offset < NVMSIZE);
  assert((uint64_t)new_addr_offset + pagesize <= NVMSIZE);

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
      assert(dst[i] == src[i]);
    }
  }
#endif
  
  gettimeofday(&start, NULL);
  newptr = libc_mmap((void*)page->va, pagesize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE | MAP_FIXED, nvmfd, new_addr_offset);
  if (newptr == MAP_FAILED) {
    perror("newptr mmap");
    assert(0);
  }
  if (newptr != (void*)page->va) {
    fprintf(stderr, "mapped address is not same as faulting address\n");
  }
  assert(page->va % HUGEPAGE_SIZE == 0);
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
    assert(0);
  }
  gettimeofday(&end, NULL);
  LOG_TIME("uffdio_register: %f s\n", elapsed(&start, &end));
  
  page->migrations_down++;
  migrations_down++;

  page->devdax_offset = nvm_offset;
  page->in_dram = false;
  //page->pa = hemem_va_to_pa(page);

#ifdef COALESCE
  migrate_to_nvm_hp((uint64_t) newptr, nvmfd, new_addr_offset);
#endif
  hemem_tlb_shootdown(page->va);

  bytes_migrated += pagesize;

  //LOG("hemem_migrate_down: new pte: %lx\n", hemem_va_to_pa(page->va));

  gettimeofday(&migrate_end, NULL);  
  LOG_TIME("hemem_migrate_down: %f s\n", elapsed(&migrate_start, &migrate_end));

  internal_call = false;
}

void hemem_wp_page(struct hemem_page *page, bool protect)
{
  uint64_t addr = page->va;
  struct uffdio_writeprotect wp;
  int ret;
  struct timeval start, end;
  uint64_t pagesize = pt_to_pagesize(page->pt);

  internal_call = true;

  LOG("hemem_wp_page: wp addr %lx pte: %lx\n", addr, hemem_va_to_pa(addr));

  assert(addr != 0);
  assert(addr % HUGEPAGE_SIZE == 0);

  gettimeofday(&start, NULL);
  wp.range.start = addr;
  wp.range.len = pagesize;
  wp.mode = (protect ? UFFDIO_WRITEPROTECT_MODE_WP : 0);
  ret = ioctl(uffd, UFFDIO_WRITEPROTECT, &wp);

  if (ret < 0) {
    perror("uffdio writeprotect");
    assert(0);
  }
  gettimeofday(&end, NULL);

  LOG_TIME("uffdio_writeprotect: %f s\n", elapsed(&start, &end));

  internal_call = false;
}


void handle_wp_fault(uint64_t page_boundry)
{
  struct hemem_page *page;

  internal_call = true;

  page = find_page(page_boundry);
  assert(page != NULL);

  migration_waits++;

  LOG("hemem: handle_wp_fault: waiting for migration for page %lx\n", page_boundry);

  pthread_mutex_lock(&(page->page_lock));

  assert(!page->migrating);

  pthread_mutex_unlock(&(page->page_lock));

  internal_call = false;
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

  internal_call = true;

  assert(page_boundry != 0);

  //LOG("missing page fault at %p\n", page_boundry);
  // have we seen this page before?
#if 0
  page = find_page(page_boundry);
  if (page != NULL) {
    // if yes, must have unmapped it for migration, wait for migration to finish
    LOG("hemem: encountered a page in the middle of migration, waiting\n");
    handle_wp_fault(page_boundry);
    return;
  }
#endif
  gettimeofday(&missing_start, NULL);
  /* internal_malloc = true; */
  assert(nextfreepage < MAXPAGES);
  
  //!! page = &freepages[nextfreepage++];
  
  //printf("getting page for pagefault at %p\n", page_boundry);
  /* page = (struct hemem_page*)calloc(1, sizeof(struct hemem_page)); */
  /* internal_malloc = false; */
  /* if (page == NULL) { */
  /*   perror("page calloc"); */
  /*   assert(0); */
  /* } */
  pthread_mutex_init(&(page->page_lock), NULL);

  gettimeofday(&missing_start, NULL);

  gettimeofday(&start, NULL);
  page->va = page_boundry; 
  
#ifdef COALESCE
  void* huge_page = check_aligned(page_boundry);

  if(huge_page) {
    LOG("aligned page fault at %p\n", page_boundry);
    aligned_pagefault(page, huge_page);
    check_in_dram(page, huge_page, dramfd, nvmfd);
  } else {
    LOG("unaligned page fault at %p\n", page_boundry);
    page = pagefault(); 
  }
#else
  page = pagefault();
#endif
  // let policy algorithm do most of the heavy lifting of finding a free page
  assert(page != NULL);
  
  gettimeofday(&end, NULL);
  LOG_TIME("page_fault: %f s\n", elapsed(&start, &end));
  
  offset = page->devdax_offset;
  in_dram = page->in_dram;
  pagesize = pt_to_pagesize(page->pt);

  tmp_offset = (in_dram) ? dram_devdax_mmap + offset : nvm_devdax_mmap + offset;

  hemem_parallel_memset(tmp_offset, 0, pagesize);
  memsets++;

#ifdef HEMEM_DEBUG
  char* tmp = (char*)tmp_offset;
  for (int i = 0; i < pagesize; i++) {
    assert(tmp[i] == 0);
  }
#endif

  // now that we have an offset determined via the policy algorithm, actually map
  // the page for the application
  gettimeofday(&start, NULL);
  //newptr = libc_mmap((void*)page_boundry, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE | MAP_FIXED, (in_dram ? dramfd : nvmfd), offset);

  newptr = libc_mmap((void*)page_boundry, pagesize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE | MAP_FIXED, (in_dram ? dramfd : nvmfd), offset);
  
  if (newptr == MAP_FAILED) {
    perror("newptr mmap");
    /* free(page); */
    assert(0);
  }
  LOG("hemem: mmaping at %p with offset %p\n", newptr, offset);
  if(newptr != (void *)page_boundry) {
    fprintf(stderr, "Not mapped where expected (%p != %p)\n", newptr, (void *)page_boundry);
    assert(0);
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
    assert(0);
  }
  gettimeofday(&end, NULL);
  LOG_TIME("uffdio_register: %f s\n", elapsed(&start, &end));

  // use mmap return addr to track new page's virtual address
  page->va = (uint64_t)newptr;
  //page->size = PAGE_SIZE;

  assert(page->va != 0);
  assert(page->va % HUGEPAGE_SIZE == 0);
  page->migrating = false;
  page->migrations_up = page->migrations_down = 0;
  //page->pa = hemem_va_to_pa(page);
 
  mem_allocated += pagesize;

  //LOG("hemem_missing_fault: va: %lx assigned to %s frame %lu  pte: %lx\n", page->va, (in_dram ? "DRAM" : "NVM"), page->devdax_offset / pagesize, hemem_va_to_pa(page->va));

  // place in hemem's page tracking list
  add_page(page);

#ifdef COALESCE  
  if(page->in_dram) incr_dram_huge_page(page->va, dramfd, offset);
  else incr_nvm_huge_page(page->va, nvmfd, offset);
#endif

  missing_faults_handled++;
  pages_allocated++;
  gettimeofday(&missing_end, NULL);
  LOG("finished page fault of %p\n", page->va);
  LOG_TIME("hemem_missing_fault: %f s\n", elapsed(&missing_start, &missing_end));

  internal_call = false;
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
      assert(0);
    case 0:
      fprintf(stderr, "poll read 0\n");
      continue;
    case 1:
      break;
    default:
      fprintf(stderr, "unexpected poll result\n");
      assert(0);
    }

    if (pollfd.revents & POLLERR) {
      fprintf(stderr, "pollerr\n");
      assert(0);
    }

    if (!pollfd.revents & POLLIN) {
      continue;
    }

    nread = read(uffd, &msg[0], MAX_UFFD_MSGS * sizeof(struct uffd_msg));
    if (nread == 0) {
      fprintf(stderr, "EOF on userfaultfd\n");
      assert(0);
    }

    if (nread < 0) {
      if (errno == EAGAIN) {
        continue;
      }
      perror("read");
      assert(0);
    }

    if ((nread % sizeof(struct uffd_msg)) != 0) {
      fprintf(stderr, "invalid msg size: [%ld]\n", nread);
      assert(0);
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
          assert(0);
        }
      }
      else if (msg[i].event & UFFD_EVENT_UNMAP){
        fprintf(stderr, "Received an unmap event\n");
        assert(0);
      }
      else if (msg[i].event & UFFD_EVENT_REMOVE) {
        fprintf(stderr, "received a remove event\n");
        assert(0);
      }
      else {
        fprintf(stderr, "received a non page fault event\n");
        assert(0);
      }
    }
  }
}

/*
uint64_t* hemem_va_to_pa(struct hemem_page *page)
{
<<<<<<< HEAD
  void * ret;
  struct hemem_page *hugepage;
  struct hemem_page *basepage;
  uint64_t hugepage_offset;
  bool in_dram;
  bool migrating;
  uint64_t hugepage_fd;

  LOG("promote\n");
  
  // ensure addr is hugepage aligned
  assert(addr % HUGEPAGE_SIZE == 0);

  // take some page properties from first base page
  basepage = find_page(addr);
  assert(basepage != NULL);
  assert(basepage->pt == BASEP);
=======
  uint64_t pt_base = ((uint64_t)(cr3 & ADDRESS_MASK));
  uint64_t pgd_entry;
  uint64_t pud_entry;
  uint64_t pmd_entry;
  uint64_t pte_entry;
  uint64_t offset;
>>>>>>> origin/wp-fault-handling

  page->pgd = (uint64_t*)libc_mmap(NULL, BASEPAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, devmemfd, pt_base);
  if (page->pgd == MAP_FAILED) {
    perror("hemem_va_to_pa pgd mmap:");
    assert(0);
  }
  offset = (((page->va) >> HEMEM_PGDIR_SHIFT) & (HEMEM_PTRS_PER_PGD - 1));
  assert(offset < BASEPAGE_SIZE);
  pgd_entry = *(page->pgd + offset) ;
  if (!((pgd_entry & HEMEM_PRESENT_FLAG) == HEMEM_PRESENT_FLAG)) {
    LOG("hemem_va_to_pa: pgd not present: %016lx\n", pgd_entry);
    assert(0);
  }

  page->pud = (uint64_t*)libc_mmap(NULL, BASEPAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, devmemfd, pgd_entry & ADDRESS_MASK);
  if (page->pud == MAP_FAILED) {
    perror("hemem_va_to_pa pud mmap:");
    assert(0);
  }
  offset =  (((page->va) >> HEMEM_PUD_SHIFT) & (HEMEM_PTRS_PER_PUD - 1));
  assert(offset < BASEPAGE_SIZE);
  pud_entry = *(page->pud + offset);
  if (!((pud_entry & HEMEM_PRESENT_FLAG) == HEMEM_PRESENT_FLAG)) {
    LOG("hemem_va_to_pa: pud not present: %016lx\n", pud_entry);
    assert(0);
  }

  page->pmd = (uint64_t*)libc_mmap(NULL, BASEPAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, devmemfd, pud_entry & ADDRESS_MASK);
  if (page->pmd == MAP_FAILED) {
    perror("hemem_va_to_pa pmd mmap:");
    assert(0);
  }
<<<<<<< HEAD

  hugepage->va = addr;
  hugepage->devdax_offset = hugepage_offset;
  hugepage->in_dram = in_dram;
  hugepage->pt = HUGEP;
  hugepage->migrating = migrating;
  pthread_mutex_init(&(hugepage->page_lock), NULL);
  hugepage->migrations_up = hugepage->migrations_down = 0;

  enqueue_page(hugepage);
}

// demote huge page starting at addr into 512 base pages
// addr is the addr of the hugepage to demote (must be hugepage aligned)
void hemem_demote_pages(uint64_t addr)
{
  void* ret;
  struct hemem_page *hugepage;
  struct hemem_page *page;
  uint64_t basepage_addr;
  uint64_t basepage_offset;
  uint32_t basepage_fd;

  LOG("demote");
  // ensure addr is start of a huge page
  assert(addr % HUGEPAGE_SIZE == 0);

  // find the hemem huge page
  hugepage = find_page(addr);
  assert(hugepage != NULL);
  assert(hugepage->pt == HUGEP);

  // remove it from hemem tracking list
  remove_page(hugepage);

  // for each base page
  for (uint64_t i = 0; i < 512; i++) {
    // grab a new page
    //TODO: Probably need lists for page sizes rather than fixed size array
    assert(nextfreepage < MAXPAGES);
    page = &freepages[nextfreepage++];

    basepage_addr = hugepage->va + (i * BASEPAGE_SIZE);
    basepage_offset = hugepage->devdax_offset + (i * BASEPAGE_SIZE);
    basepage_fd = (hugepage->in_dram) ? dramfd : nvmfd;

    ret = libc_mmap((void*)basepage_addr, BASEPAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE | MAP_FIXED, basepage_fd, basepage_offset);
    if (ret == MAP_FAILED) {
      perror("demote pages mmap");
      assert(0);
    }
    if (ret != (void*)basepage_addr) {
      fprintf(stderr, "demote pages: not mapped where expected: %p != %p\n", ret, (void*)basepage_addr);
    }

    // re-register wiht userfault
    struct uffdio_register uffdio_register;
    uffdio_register.range.start = basepage_addr;
    uffdio_register.range.len = BASEPAGE_SIZE;
    uffdio_register.mode = UFFDIO_REGISTER_MODE_MISSING | UFFDIO_REGISTER_MODE_WP;
    uffdio_register.ioctls = 0;
    if (ioctl(uffd, UFFDIO_REGISTER, &uffdio_register) == -1) {
      perror("demote pages ioctl uffdio_register");
      assert(0);
    }

    page->va = basepage_addr;
    page->devdax_offset = basepage_offset;
    page->in_dram = hugepage->in_dram;
    page->pt = BASEP;
    page->migrating = hugepage->migrating;
    pthread_mutex_init(&(page->page_lock), NULL);
    page->migrations_up = page->migrations_down = 0;

    // add base page to hemem tracking list
    enqueue_page(page);
=======
  offset = (((page->va) >> HEMEM_PMD_SHIFT) & (HEMEM_PTRS_PER_PMD - 1));
  assert(offset < BASEPAGE_SIZE);
  pmd_entry = *(page->pmd + offset);
  if (!((pmd_entry & HEMEM_PRESENT_FLAG) == HEMEM_PRESENT_FLAG)) {
    LOG("hemem_va_to_pa: pmd not present: %016lx\n", pmd_entry);
    assert(0);
>>>>>>> origin/wp-fault-handling
  }

  if ((pmd_entry & HEMEM_HUGEPAGE_FLAG) == HEMEM_HUGEPAGE_FLAG) {
    return (page->pmd + offset);
  }

  page->pte = (uint64_t*)libc_mmap(NULL, BASEPAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, devmemfd, pmd_entry & ADDRESS_MASK);
  if (page->pte == MAP_FAILED) {
    perror("hemem_va_to_pa pte mmap:");
    assert(0);
  }
  offset = (((page->va) >> HEMEM_PAGE_SHIFT) & (HEMEM_PTRS_PER_PTE - 1));
  assert(offset < BASEPAGE_SIZE);
  pte_entry = *(page->pte + offset);
  if (!((pte_entry & HEMEM_PRESENT_FLAG) == HEMEM_PRESENT_FLAG)) {
    LOG("hemem_va_to_pa: pte not present: %016lx\n", pte_entry);
    assert(0);
  }
  
  return (page->pte + offset);
}
*/

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
/*
void hemem_clear_bits(struct hemem_page *page)
{
  uint64_t page_boundry = page->va & ~(PAGE_SIZE - 1);
  clear_bits(page_boundry);
}

void hemem_huge_tlb_shootdown(uint64_t va)
{
  uint64_t page_boundry = va & ~(PAGE_SIZE - 1);
  struct uffdio_range range;
  int ret;

  range.start = page_boundry;
  range.len = HUGEPAGE_SIZE;

  ret = ioctl(uffd, UFFDIO_TLBFLUSH, &range);
  if (ret < 0) {
    perror("uffdio tlbflush");
    assert(0);
  }
}



uint64_t hemem_get_bits(struct hemem_page *page)
{
  uint64_t page_boundry = page->va & ~(PAGE_SIZE - 1);
  return get_bits(page_boundry);
}
*/

void hemem_clear_bits(struct hemem_page *page)
{
  uint64_t ret;
  struct uffdio_page_flags page_flags;

  page_flags.va = page->va;
  assert(page_flags.va % HUGEPAGE_SIZE == 0);
  page_flags.flag1 = HEMEM_ACCESSED_FLAG;
  page_flags.flag2 = HEMEM_DIRTY_FLAG;

  if (ioctl(uffd, UFFDIO_CLEAR_FLAG, &page_flags) < 0) {
    fprintf(stderr, "userfaultfd_clear_flag returned < 0\n");
    assert(0);
  }

  ret = page_flags.res1;
  if (ret == 0) {
    LOG("hemem_clear_accessed_bit: accessed bit not cleared\n");
  }
}


uint64_t hemem_get_bits(struct hemem_page *page)
{
  uint64_t ret;
  struct uffdio_page_flags page_flags;

  page_flags.va = page->va;
  assert(page_flags.va % HUGEPAGE_SIZE == 0);
  page_flags.flag1 = HEMEM_ACCESSED_FLAG;
  page_flags.flag2 = HEMEM_DIRTY_FLAG;

  if (ioctl(uffd, UFFDIO_GET_FLAG, &page_flags) < 0) {
    fprintf(stderr, "userfaultfd_get_flag returned < 0\n");
    assert(0);
  }

  ret = page_flags.res1 | page_flags.res2;

  return ret;;
}

void hemem_print_stats()
{
  //fprintf(stderr, "mem_allocated: [%lu]\tmissing_faults_handled: [%lu]\tmigrations_up: [%lu]\tmigrations_down: [%lu]\tpmemcpys: [%lu]\tmemsets: [%lu]\tnextfreepage[%lu]\n", mem_allocated, missing_faults_handled, migrations_up, migrations_down, pmemcpys, memsets, nextfreepage);

  LOG_STATS("mem_allocated: [%lu]\tpages_allocated: [%lu]\tmissing_faults_handled: [%lu]\tbytes_migrated: [%lu]\tmigrations_up: [%lu]\tmigrations_down: [%lu]\tmigration_waits: [%lu]\n", 
               mem_allocated, 
               pages_allocated, 
               missing_faults_handled, 
               bytes_migrated,
               migrations_up, 
               migrations_down,
               migration_waits);
   mmgr_stats(); 
}


void hemem_clear_stats()
{
  pages_allocated = 0;
  pages_freed = 0;
  missing_faults_handled = 0;
  migrations_up = 0;
  migrations_down = 0;
}


struct hemem_page* get_hemem_page(uint64_t va)
{
  return find_page(va);
}

void hemem_start_timing(void)
{
  timing = true;
}

void hemem_stop_timing(void)
{
  timing = false;
}
