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
static bool init = false;
_Atomic uint64_t mem_allocated = 0;
_Atomic uint64_t fastmem_allocated = 0;
_Atomic uint64_t slowmem_allocated = 0;
_Atomic uint64_t wp_faults_handled = 0;
_Atomic uint64_t missing_faults_handled = 0;
_Atomic uint64_t migrations_up = 0;
_Atomic uint64_t migrations_down = 0;
_Atomic uint64_t pmemcpys = 0;
_Atomic uint64_t memsets = 0;
uint64_t base = 0;
int devmemfd = -1;
struct page_list list;
pthread_t copy_threads[MAX_COPY_THREADS];

void *dram_devdax_mmap;
void *nvm_devdax_mmap;

struct pmemcpy {
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
  uint64_t *tmp1, *tmp2;
  uint64_t i;

  assert(tid < MAX_COPY_THREADS);

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

  tmp1 = dst;
  tmp2 = src;
  for (i = 0; i < chunk_size / sizeof(uint64_t); i++) {
    if (tmp1[i] != tmp2[i]) {
      LOG("copy thread: dst[%lu] = %lu != src[%lu] = %lu\n", i, tmp1[i], i, tmp2[i]);
      assert(tmp1[i] == tmp2[i]);
    }
  }
  return NULL;
}


void enqueue_page(struct hemem_page *page)
{
  assert(page->prev == NULL);
  page->next = list.first;
  if (list.first != NULL) {
    assert(list.first->prev == NULL);
    list.first->prev = page;
  }
  else {
    assert(list.last == NULL);
    assert(list.numentries == 0);
    list.last = page;
  }
  list.first = page;
  list.numentries++;
}


struct hemem_page* find_page(uint64_t va)
{
  struct hemem_page *cur = list.first;

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

  uffd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
  if (uffd == -1) {
    perror("uffd");
    assert(0);
  }

  uffdio_api.api = UFFD_API;
  uffdio_api.features = UFFD_FEATURE_PAGEFAULT_FLAG_WP |  UFFD_FEATURE_MISSING_SHMEM | UFFD_FEATURE_MISSING_HUGETLBFS;
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

  devmemfd = open("/dev/mem", O_RDWR | O_SYNC);
  if (devmemfd < 0) {
    perror("/dev/mem open");
    assert(0);
  }

  hememlogf = fopen("logs.txt", "w+");
  if (hememlogf == NULL) {
    perror("log file open\n");
    assert(0);
  }

  timef = fopen("times.txt", "w+");
  if (timef == NULL) {
    perror("time file fopen\n");
    assert(0);
  }

  dram_devdax_mmap =libc_mmap(NULL, DRAMSIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, dramfd, 0);
  if (dram_devdax_mmap == MAP_FAILED) {
    perror("dram devdax mmap");
    assert(0);
  }

  nvm_devdax_mmap =libc_mmap(NULL, NVMSIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, nvmfd, 0);
  if (nvm_devdax_mmap == MAP_FAILED) {
    perror("nvm devdax mmap");
    assert(0);
  }
  
  paging_init();

  init = true;
}

static void hemem_mmap_populate(void* addr, size_t length)
{
  // Page mising fault case - probably the first touch case
  // allocate in DRAM via LRU
  void* newptr;
  uint64_t offset;
  struct hemem_page *page;
  bool in_dram;
  void* page_boundry;
  uint64_t npages;
  int i;


  npages = length / PAGE_SIZE;
  page_boundry = addr;

  LOG("hemem_mmap_populate: addr: 0x%lx, npages: %lu\n", (uint64_t)addr, npages);
  for (i = 0; i < npages; i++) {
    page = (struct hemem_page*)calloc(1, sizeof(struct hemem_page));
    if (page == NULL) {
      perror("page calloc");
      assert(0);
    }

    // let policy algorithm do most of the heavy lifting of finding a free page
    pagefault(page); 
    offset = page->devdax_offset;
    in_dram = page->in_dram;
  
    // now that we have an offset determined via the policy algorithm, actually map
    // the page for the application
    newptr = libc_mmap((void*)page_boundry, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE | MAP_FIXED, (in_dram ? dramfd : nvmfd), offset);
    if (newptr == MAP_FAILED) {
      perror("newptr mmap");
      free(page);
      assert(0);
    }
  
    if (newptr != (void*)page_boundry) {
      printf("hemem: mmap populate: warning, newptr != page boundry\n");
    }

    memset(newptr, 0, PAGE_SIZE);
    memsets++;

    // re-register new mmap region with userfaultfd
    struct uffdio_register uffdio_register;
    uffdio_register.range.start = (uint64_t)newptr;
    uffdio_register.range.len = PAGE_SIZE;
    uffdio_register.mode = UFFDIO_REGISTER_MODE_MISSING | UFFDIO_REGISTER_MODE_WP;
    uffdio_register.ioctls = 0;
    if (ioctl(uffd, UFFDIO_REGISTER, &uffdio_register) == -1) {
      perror("ioctl uffdio_register");
      assert(0);
    }

    // use mmap return addr to track new page's virtual address
    page->va = (uint64_t)newptr;
    page->migrating = false;
 
    pthread_mutex_init(&(page->page_lock), NULL);

    mem_allocated += PAGE_SIZE;

    // place in hemem's page tracking list
    enqueue_page(page);
    page_boundry += PAGE_SIZE;
  }
}


void* hemem_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
  void *p;
  struct uffdio_base uffdio_base;
 
  assert(init);

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
  p = libc_mmap(addr, length, prot, flags, dramfd, offset);
  if (p == NULL || p == MAP_FAILED) {
    perror("mmap");
  }  
  assert(p != NULL && p != MAP_FAILED);

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

  uffdio_base.range.start = (uint64_t)p;
  uffdio_base.range.len = length;

  if (ioctl(uffd, UFFDIO_BASE, &uffdio_base) < 0) {
    perror("ioctl uffdio_base");
    assert(0);
  }
  base = uffdio_base.base;
  
  if ((flags & MAP_POPULATE) == MAP_POPULATE) {
    hemem_mmap_populate(p, length);
  }
  
  return p;
}


int hemem_munmap(void* addr, size_t length)
{
  return libc_munmap(addr, length);
}


static void hemem_parallel_memcpy(void *dst, void *src, size_t length)
{
  uint64_t i;
  pthread_t copy_threads[MAX_COPY_THREADS];
  int r;

  pmemcpy.dst = dst;
  pmemcpy.src = src;
  pmemcpy.length = length;

  for (i = 0; i < MAX_COPY_THREADS; i++) {
    r = pthread_create(&copy_threads[i], NULL, hemem_parallel_memcpy_thread, (void*)i);
    assert(r == 0);
  }
  for (i = 0; i < MAX_COPY_THREADS; i++) {
    r = pthread_join(copy_threads[i], NULL);
    assert(r == 0);
  }
}


void hemem_migrate_up(struct hemem_page *page, uint64_t dram_framenum)
{
  void *old_addr;
  void *new_addr;
  void *newptr;
  struct timeval migrate_start, migrate_end;
  struct timeval start, end;
  uint64_t old_addr_offset, new_addr_offset;

  LOG("hemem_migrate_up: migrate down addr: %lx pte: %lx\n", page->va, hemem_va_to_pa(page->va));
  
  gettimeofday(&migrate_start, NULL);
  
  assert(page != NULL);
  old_addr_offset = page->devdax_offset;
  new_addr_offset = dram_framenum * PAGE_SIZE;

  old_addr = nvm_devdax_mmap + old_addr_offset;
  assert((uint64_t)old_addr_offset < NVMSIZE);
  assert((uint64_t)old_addr_offset + PAGE_SIZE <= NVMSIZE);

  new_addr = dram_devdax_mmap + new_addr_offset;
  assert((uint64_t)new_addr_offset < DRAMSIZE);
  assert((uint64_t)new_addr_offset + PAGE_SIZE <= DRAMSIZE);

  // copy page from faulting location to temp location
  gettimeofday(&start, NULL);
  hemem_parallel_memcpy(new_addr, old_addr, PAGE_SIZE);
  gettimeofday(&end, NULL);
  LOG_TIME("memcpy_to_dram: %f s\n", elapsed(&start, &end));
  
  uint64_t* src = (uint64_t*)old_addr;
  uint64_t* dst = (uint64_t*)new_addr;
  for (int i = 0; i < (PAGE_SIZE / sizeof(uint64_t)); i++) {
    if (dst[i] != src[i]) {
      LOG("hemem_migrate_up: dst[%d] = %lu != src[%d] = %lu\n", i, dst[i], i, src[i]);
      assert(dst[i] == src[i]);
    }
  }

  gettimeofday(&start, NULL);
  newptr = libc_mmap((void*)page->va, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE | MAP_FIXED, dramfd, new_addr_offset);
  if (newptr == MAP_FAILED) {
    perror("newptr mmap");
    assert(0);
  }
  if (newptr != (void*)page->va) {
    printf("mapped address is not same as faulting address\n");
  }
  gettimeofday(&end, NULL);
  LOG_TIME("mmap_dram: %f s\n", elapsed(&start, &end));

  /* for (int i = 0; i < (PAGE_SIZE / sizeof(uint64_t)); i++) { */
  /*   if (dst[i] != src[i]) { */
  /*     LOG("hemem_migrate_up: dst[%d] = %lu != src[%d] = %lu\n", i, dst[i], i, src[i]); */
  /*     assert(dst[i] == src[i]); */
  /*   } */
  /* } */

  // re-register new mmap region with userfaultfd
  gettimeofday(&start, NULL);
  struct uffdio_register uffdio_register;
  uffdio_register.range.start = (uint64_t)newptr;
  uffdio_register.range.len = PAGE_SIZE;
  uffdio_register.mode = UFFDIO_REGISTER_MODE_MISSING | UFFDIO_REGISTER_MODE_WP;
  uffdio_register.ioctls = 0;
  if (ioctl(uffd, UFFDIO_REGISTER, &uffdio_register) == -1) {
    perror("ioctl uffdio_register");
    assert(0);
  }
  gettimeofday(&end, NULL);
  LOG_TIME("uffdio_register: %f s\n", elapsed(&start, &end));
  
  /* uint64_t* new = (uint64_t*)newptr; */
  /* for (int i = 0; i < (PAGE_SIZE / sizeof(uint64_t)); i++) { */
  /*   if (new[i] != src[i]) { */
  /*     LOG("hemem_migrate_up: new[%d] = 0x%" PRIx64" != src[%d] = 0x%" PRIx64" (dst[%d] = 0x%" PRIx64")\n", i, new[i], i, src[i], i, dst[i]); */
  /*     LOG("hemem_migrate_up: address of new[%d]: %lx, page: %lx\n", i, (uint64_t)&new[i], ((uint64_t)&new[i] & ~(PAGE_SIZE - 1))); */
  /*     assert(new[i] == src[i]); */
  /*   } */
  /* } */

  page->migrations_up++;
  migrations_up++;

  page->devdax_offset = dram_framenum * PAGE_SIZE;
  page->in_dram = true;

  hemem_tlb_shootdown(page->va);
  
  LOG("hemem_migrate_up: new pte: %lx\n", hemem_va_to_pa(page->va));

  gettimeofday(&migrate_end, NULL);  
  LOG_TIME("hemem_migrate_up: %f s\n", elapsed(&migrate_start, &migrate_end));
}


void hemem_migrate_down(struct hemem_page *page, uint64_t nvm_framenum)
{
  void *old_addr;
  void *new_addr;
  void *newptr;
  struct timeval migrate_start, migrate_end;
  struct timeval start, end;
  uint64_t old_addr_offset, new_addr_offset;

  LOG("hemem_migrate_down: migrate down addr: %lx pte: %lx\n", page->va, hemem_va_to_pa(page->va));

  gettimeofday(&migrate_start, NULL);
  
  assert(page != NULL);
  old_addr_offset = page->devdax_offset;
  new_addr_offset = nvm_framenum * PAGE_SIZE;

  old_addr = dram_devdax_mmap + old_addr_offset;
  assert((uint64_t)old_addr_offset < DRAMSIZE);
  assert((uint64_t)old_addr_offset + PAGE_SIZE <= DRAMSIZE);

  new_addr = nvm_devdax_mmap + new_addr_offset;
  assert((uint64_t)new_addr_offset < NVMSIZE);
  assert((uint64_t)new_addr_offset + PAGE_SIZE <= NVMSIZE);

  // copy page from faulting location to temp location
  gettimeofday(&start, NULL);
  hemem_parallel_memcpy(new_addr, old_addr, PAGE_SIZE);
  gettimeofday(&end, NULL);
  LOG_TIME("memcpy_to_nvm: %f s\n", elapsed(&start, &end));

  uint64_t* src = (uint64_t*)old_addr;
  uint64_t* dst = (uint64_t*)new_addr;
  for (int i = 0; i < (PAGE_SIZE / sizeof(uint64_t)); i++) {
    if (dst[i] != src[i]) {
      LOG("hemem_migrate_down: dst[%d] = %lu != src[%d] = %lu\n", i, dst[i], i, src[i]);
      assert(dst[i] == src[i]);
    }
  }
  
  gettimeofday(&start, NULL);
  newptr = libc_mmap((void*)page->va, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE | MAP_FIXED, nvmfd, new_addr_offset);
  if (newptr == MAP_FAILED) {
    perror("newptr mmap");
    assert(0);
  }
  if (newptr != (void*)page->va) {
    printf("mapped address is not same as faulting address\n");
  }
  gettimeofday(&end, NULL);
  LOG_TIME("mmap_nvm: %f s\n", elapsed(&start, &end));

  // re-register new mmap region with userfaultfd
  gettimeofday(&start, NULL);
  struct uffdio_register uffdio_register;
  uffdio_register.range.start = (uint64_t)newptr;
  uffdio_register.range.len = PAGE_SIZE;
  uffdio_register.mode = UFFDIO_REGISTER_MODE_MISSING | UFFDIO_REGISTER_MODE_WP;
  uffdio_register.ioctls = 0;
  if (ioctl(uffd, UFFDIO_REGISTER, &uffdio_register) == -1) {
    perror("ioctl uffdio_register");
    assert(0);
  }
  gettimeofday(&end, NULL);
  LOG_TIME("uffdio_register: %f s\n", elapsed(&start, &end));
  
  /* uint64_t* new = (uint64_t*)newptr; */
  /* for (int i = 0; i < (PAGE_SIZE / sizeof(uint64_t)); i++) { */
  /*   if (new[i] != src[i]) { */
  /*     LOG("hemem_migrate_down: new[%d] = %lu != src[%d] = %lu(dst[%d] = %lu)\n", i, new[i], i, src[i], i, dst[i]); */
  /*     LOG("hemem_migrate_down: address of new[%d]: %lx, page: %lx\n", i, (uint64_t)&new[i], ((uint64_t)&new[i] & ~(PAGE_SIZE - 1))); */
  /*     assert(new[i] == src[i]); */
  /*   } */
  /* } */
  
  page->migrations_down++;
  migrations_down++;

  page->devdax_offset = nvm_framenum * PAGE_SIZE;
  page->in_dram = false;

  hemem_tlb_shootdown(page->va);

  LOG("hemem_migrate_down: new pte: %lx\n", hemem_va_to_pa(page->va));

  gettimeofday(&migrate_end, NULL);  
  LOG_TIME("hemem_migrate_down: %f s\n", elapsed(&migrate_start, &migrate_end));
}

void hemem_wp_page(struct hemem_page *page, bool protect)
{
  uint64_t addr = page->va;
  struct uffdio_writeprotect wp;
  int ret;
  struct timeval start, end;

  LOG("hemem_wp_page: wp addr %lx pte: %lx\n", addr, hemem_va_to_pa(addr));
/*
  // unmap page entirely during migration to prevent any access by process
 LOG("Write protect va: 0x%lx\n", addr);
  ret = libc_munmap((void*)addr, PAGE_SIZE);
  if (ret < 0) {
    perror("munmap\n");
    assert(0);
  }
  */

  gettimeofday(&start, NULL);
  wp.range.start = addr;
  wp.range.len = PAGE_SIZE;
  wp.mode = (protect ? UFFDIO_WRITEPROTECT_MODE_WP : 0);
  ret = ioctl(uffd, UFFDIO_WRITEPROTECT, &wp);

  if (ret < 0) {
    perror("uffdio writeprotect");
    assert(0);
  }
  gettimeofday(&end, NULL);

  hemem_tlb_shootdown(page->va);
 
  LOG_TIME("uffdio_writeprotect: %f s\n", elapsed(&start, &end));
}


void handle_wp_fault(uint64_t page_boundry)
{
  //assert(!"wp fault handling not yet implemented\n");
  struct hemem_page *page;

  //assert(!"NYI");

  page = find_page(page_boundry);
  assert(page != NULL);

  LOG("hemem: handle_wp_fault: waiting for migration for page %lx\n", page_boundry);

  pthread_mutex_lock(&(page->page_lock));

  assert(!page->migrating);

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
  bool in_dram;

  // have we seen this page before?
  page = find_page(page_boundry);
  if (page != NULL) {
    // if yes, must have unmapped it for migration, wait for migration to finish
    LOG("hemem: encountered a page in the middle of migration, waiting\n");
    handle_wp_fault(page_boundry);
    return;
  }

  gettimeofday(&missing_start, NULL);
  page = (struct hemem_page*)calloc(1, sizeof(struct hemem_page));
  if (page == NULL) {
    perror("page calloc");
    assert(0);
  }
  pthread_mutex_init(&(page->page_lock), NULL);

  // let policy algorithm do most of the heavy lifting of finding a free page
  gettimeofday(&start, NULL);
  pagefault(page); 
  gettimeofday(&end, NULL);
  LOG_TIME("page_fault: %f s\n", elapsed(&start, &end));
  offset = page->devdax_offset;
  in_dram = page->in_dram;
  
  // now that we have an offset determined via the policy algorithm, actually map
  // the page for the application
  gettimeofday(&start, NULL);
  newptr = libc_mmap((void*)page_boundry, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE | MAP_FIXED, (in_dram ? dramfd : nvmfd), offset);
  if (newptr == MAP_FAILED) {
    perror("newptr mmap");
    free(page);
    assert(0);
  }
  gettimeofday(&end, NULL);
  LOG_TIME("mmap_%s: %f s\n", (in_dram ? "dram" : "nvm"), elapsed(&start, &end));
  
  if (newptr != (void*)page_boundry) {
    printf("hemem: handle missing fault: warning, newptr != page boundry\n");
  }

  memset(newptr, 0, PAGE_SIZE);
  memsets++;

  char* tmp = (char*)newptr;
  for (int i = 0; i < PAGE_SIZE; i++) {
    assert(tmp[i] == 0);
  }

  gettimeofday(&start, NULL);
  // re-register new mmap region with userfaultfd
  struct uffdio_register uffdio_register;
  uffdio_register.range.start = (uint64_t)newptr;
  uffdio_register.range.len = PAGE_SIZE;
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
  page->migrating = false;
 
  pthread_mutex_init(&(page->page_lock), NULL);

  mem_allocated += PAGE_SIZE;

  LOG("hemem_missing_fault: va: %lx assigned to %s frame %lu  pte: %lx\n", page->va, (in_dram ? "DRAM" : "NVM"), page->devdax_offset / PAGE_SIZE, hemem_va_to_pa(page->va));

  // place in hemem's page tracking list
  enqueue_page(page);

  missing_faults_handled++;
  gettimeofday(&missing_end, NULL);
  LOG_TIME("hemem_missing_fault: %f s\n", elapsed(&missing_start, &missing_end));
  //printf("page missing fault took %.4f seconds\n", elapsed(&start, &end));
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

  //printf("fault handler entered\n");

  for (;;) {
    struct pollfd pollfd;
    int pollres;
    pollfd.fd = uffd;
    pollfd.events = POLLIN;

    //printf("calling poll\n");
    pollres = poll(&pollfd, 1, -1);
    //printf("poll returned\n");

    switch (pollres) {
    case -1:
      perror("poll");
      assert(0);
    case 0:
      printf("poll read 0\n");
      continue;
    case 1:
      //printf("poll read 1\n");
      break;
    default:
      printf("unexpected poll result\n");
      assert(0);
    }

    if (pollfd.revents & POLLERR) {
      printf("pollerr\n");
      assert(0);
    }

    if (!pollfd.revents & POLLIN) {
      continue;
    }

    nread = read(uffd, &msg[0], MAX_UFFD_MSGS * sizeof(struct uffd_msg));
    if (nread == 0) {
      printf("EOF on userfaultfd\n");
      assert(0);
    }

    if (nread < 0) {
      if (errno == EAGAIN) {
        continue;
      }
      perror("read");
      assert(0);
    }

    //printf("nread: %d\tsize of uffd msg: %d\n", nread, sizeof(msg));
    if ((nread % sizeof(struct uffd_msg)) != 0) {
      printf("invalid msg size: [%ld]\n", nread);
      assert(0);
    }

    nmsgs = nread / sizeof(struct uffd_msg);

    for (i = 0; i < nmsgs; i++) {
      //TODO: check page fault event, handle it
      if (msg[i].event & UFFD_EVENT_PAGEFAULT) {
        //printf("received a page fault event\n");
        fault_addr = (uint64_t)msg[i].arg.pagefault.address;
        fault_flags = msg[i].arg.pagefault.flags;

        // allign faulting address to page boundry
        // huge page boundry in this case due to dax allignment
        page_boundry = fault_addr & ~(PAGE_SIZE - 1);
        //printf("page boundry is 0x%lx\n", page_boundry);

        if (fault_flags & UFFD_PAGEFAULT_FLAG_WP) {
          //printf("received a write-protection fault at addr 0x%lx\n", fault_addr);
          handle_wp_fault(page_boundry);
        }
        else {
          handle_missing_fault(page_boundry);
        }

        //printf("waking thread\n");
        // wake the faulting thread
        range.start = (uint64_t)page_boundry;
        range.len = PAGE_SIZE;

        ret = ioctl(uffd, UFFDIO_WAKE, &range);

        if (ret < 0) {
          perror("uffdio wake");
          assert(0);
        }
      }
      else {
        printf("Received a non page fault event\n");
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
 
  MEM_BARRIER(); 
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
  printf("missing_faults_handled: [%lu]\tmigrations_up: [%lu]\tmigrations_down: [%lu]\tpmemcpys: [%lu]\tmemsets: [%lu]\n", missing_faults_handled, migrations_up, migrations_down, pmemcpys, memsets);
}


void hemem_clear_stats()
{
  missing_faults_handled = 0;
  migrations_up = 0;
  migrations_down = 0;
  pmemcpys = 0;
  memsets = 0;
}