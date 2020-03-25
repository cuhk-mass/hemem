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

pthread_t fault_thread;

int dramfd = -1;
int nvmfd = -1;
long uffd = -1;
int init = 0;
uint64_t mem_allocated = 0;
uint64_t fastmem_allocated = 0;
uint64_t slowmem_allocated = 0;
int wp_faults_handled = 0;
int missing_faults_handled = 0;
uint64_t base = 0;
int devmemfd = -1;
struct page_list list;
uint64_t runtime = 0;


void
hemem_init()
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


  //close(nvmfd);
  //close(dramfd);

  paging_init();
#ifdef COALESCE
  printf("coalesce_init\n");
  coalesce_init();
#endif
  init = 1;
}


void* 
hemem_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
  void* p;
  struct uffdio_base uffdio_base;
  
  assert(init);

  // reserve block of memory
  p = mmap(addr, length, prot, MAP_SHARED, dramfd, offset);
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
  return p;
}


int
hemem_munmap(void* addr, size_t length)
{
  return munmap(addr, length);
}


void 
enqueue_page(struct hemem_page *page)
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


struct hemem_page*
find_page(uint64_t va)
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


void
hemem_migrate_up(struct hemem_page *page, uint64_t dram_offset)
{
  void* old_addr;
  void* new_addr;
  void* newptr;
  struct timeval start, end;
  uint64_t old_addr_offset, new_addr_offset;

  gettimeofday(&start, NULL);
  
  assert(page != NULL);
  old_addr_offset = page->devdax_offset;
  new_addr_offset = dram_offset;

  old_addr = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, nvmfd, old_addr_offset);
  new_addr = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, dramfd, new_addr_offset);

  if (old_addr == MAP_FAILED) {
    perror("old addr mmap");
    assert(0);
  }
  if (new_addr == MAP_FAILED) {
    perror("new addr mmap");
    assert(0);
  }

  // copy page from faulting location to temp location
  memcpy(new_addr, old_addr, PAGE_SIZE);

  newptr = mmap((void*)page->va, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE | MAP_FIXED, dramfd, new_addr_offset);
  if (newptr == MAP_FAILED) {
    perror("newptr mmap");
    assert(0);
  }
  if (newptr != (void*)page->va) {
    printf("mapped address is not same as faulting address\n");
  }

  munmap(old_addr, PAGE_SIZE);
  munmap(new_addr, PAGE_SIZE);
#ifdef COALESCE
  migrate_to_dram_hp(newptr, dramfd, new_addr_offset);
#endif

  gettimeofday(&end, NULL);  
}


void
hemem_migrate_down(struct hemem_page *page, uint64_t nvm_offset)
{
  void* old_addr;
  void* new_addr;
  void* newptr;
  struct timeval start, end;
  uint64_t old_addr_offset, new_addr_offset;

  gettimeofday(&start, NULL);
  
  assert(page != NULL);
  old_addr_offset = page->devdax_offset;
  new_addr_offset = nvm_offset;

  old_addr = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, dramfd, old_addr_offset);
  new_addr = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, nvmfd, new_addr_offset);

  if (old_addr == MAP_FAILED) {
    perror("old addr mmap");
    assert(0);
  }
  if (new_addr == MAP_FAILED) {
    perror("new addr mmap");
    assert(0);
  }

  // copy page from faulting location to temp location
  memcpy(new_addr, old_addr, PAGE_SIZE);

  newptr = mmap((void*)page->va, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE | MAP_FIXED, nvmfd, new_addr_offset);
  if (newptr == MAP_FAILED) {
    perror("newptr mmap");
    assert(0);
  }
  if (newptr != (void*)page->va) {
    printf("mapped address is not same as faulting address\n");
  }

  munmap(old_addr, PAGE_SIZE);
  munmap(new_addr, PAGE_SIZE);

#ifdef COALESCE
  migrate_to_nvm_hp(newptr, nvmfd, new_addr_offset);
#endif
  gettimeofday(&end, NULL);  
}


void
handle_wp_fault(uint64_t page_boundry)
{
  /*
  void* old_addr;
  void* new_addr;
  void* newptr;
  struct timeval start, end;
  int nvm_to_dram = 0;
  struct hemem_page *page;
  uint64_t old_addr_offset, new_addr_offset;

  gettimeofday(&start, NULL);

  // map virtual address to dax file offset

  page = find_page(page_boundry);
  if (page == NULL) {
    printf("handle_wp_fault: page == NULL\n");
    assert(0);
  }
  old_addr_offset = page->devdax_offset;
  nvm_to_dram = !(page->in_dram);
  //printf("page boundry: 0x%llx\tcalculated offset in dax file: 0x%llx\n", page_boundry, offset);

  new_addr_offset = (nvm_to_dram) ? fastmem_allocated : slowmem_allocated;

  if (nvm_to_dram) {
    old_addr = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, nvmfd, old_addr_offset);
    new_addr = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, dramfd, new_addr_offset);
  }
  else {
    old_addr = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, dramfd, old_addr_offset);
    new_addr = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, nvmfd, new_addr_offset);
  }

  if (old_addr == MAP_FAILED) {
    perror("old addr mmap");
    assert(0);
  }
  if (new_addr == MAP_FAILED) {
    perror("new addr mmap");
    assert(0);
  }

  // copy page from faulting location to temp location
  memcpy(new_addr, old_addr, PAGE_SIZE);

  if (nvm_to_dram) {
    newptr = mmap((void*)page_boundry, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE | MAP_FIXED, dramfd, new_addr_offset);
  }
  else {
    newptr = mmap((void*)page_boundry, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE | MAP_FIXED, nvmfd, new_addr_offset);
  }
  if (newptr == MAP_FAILED) {
    perror("newptr mmap");
    assert(0);
  }
  if (newptr != (void*)page_boundry) {
    printf("mapped address is not same as faulting address\n");
  }

  munmap(old_addr, PAGE_SIZE);
  munmap(new_addr, PAGE_SIZE);

  gettimeofday(&end, NULL);
  
  wp_faults_handled++;
*/

/*
  if (wp_faults_handled % 1000 == 0) {
    printf("write protection fault took %.6f seconds\n", elapsed(&start, &end));
  }
*/
}


void
handle_missing_fault(uint64_t page_boundry)
{
  // Page mising fault case - probably the first touch case
  // allocate in DRAM via LRU
  void* newptr;
  struct timeval start, end;
  struct hemem_page *page;
  uint64_t offset;

  gettimeofday(&start, NULL);
  page = (struct hemem_page*)calloc(1, sizeof(struct hemem_page));

  // let policy algorithm do most of the heavy lifting of finding a free page
  page->va = page_boundry; 
  
#ifdef COALESCE
  void* huge_page = check_aligned(page_boundry);

  if(huge_page) {
    aligned_pagefault(page, huge_page);
    check_in_dram(page, huge_page, dramfd, nvmfd);
  } else {
    pagefault(page); 
  }
#else
  pagefault(page);
#endif

  offset = page->devdax_offset;
  
  // now that we have an offset determined via the policy algorithm, actually map
  // the page for the application

  newptr = mmap((void*)page_boundry, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE | MAP_FIXED, (page->in_dram ? dramfd : nvmfd), offset);

  if (newptr == MAP_FAILED) {
    perror("newptr mmap");
    free(page);
    assert(0);
  }

  if (newptr != (void*)page_boundry) {
    printf("hemem: handle missing fault: warning, newptr != page boundry\n");
  }

  // use mmap return addr to track new page's virtual address
  page->va = (uint64_t)newptr;
  
#ifdef COALESCE  
  if(page->in_dram) incr_dram_huge_page(page->va, dramfd, offset);
  else incr_nvm_huge_page(page->va, nvmfd, offset);
#endif

  mem_allocated += PAGE_SIZE;
  
  // place in hemem's page tracking list 
  enqueue_page(page);

  gettimeofday(&end, NULL);
  missing_faults_handled++;
  //printf("page missing fault took %.4f seconds\n", elapsed(&start, &end));
}


void 
*handle_fault()
{
  static struct uffd_msg msg;
  ssize_t nread;
  uint64_t fault_addr;
  uint64_t fault_flags;
  uint64_t page_boundry;
  struct uffdio_range range;
  int ret;

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

    nread = read(uffd, &msg, sizeof(msg));
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
    if (nread != sizeof(msg)) {
      printf("invalid msg size\n");
      assert(0);
    }

    //TODO: check page fault event, handle it
    if (msg.event & UFFD_EVENT_PAGEFAULT) {
      //printf("received a page fault event\n");
      fault_addr = (uint64_t)msg.arg.pagefault.address;
      fault_flags = msg.arg.pagefault.flags;

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


uint64_t 
hemem_va_to_pa(uint64_t va)
{
  uint64_t page_boundry = va & ~(PAGE_SIZE - 1);
  return va_to_pa(page_boundry);
}


void 
hemem_clear_accessed_bit(uint64_t va)
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


int 
hemem_get_accessed_bit(uint64_t va)
{
  uint64_t page_boundry = va & ~(PAGE_SIZE - 1);

  return get_accessed_bit(page_boundry);
}
