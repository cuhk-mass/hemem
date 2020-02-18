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
uint64_t mem_allocated = 0;
uint64_t fastmem_allocated = 0;
uint64_t slowmem_allocated = 0;
uint64_t wp_faults_handled = 0;
uint64_t missing_faults_handled = 0;
uint64_t base = 0;
int devmemfd = -1;
struct page_list list;
bool base_set = false;

void *dram_devdax_mmap;
void *nvm_devdax_mmap;


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

  timef = fopen("logs/times.txt", "w+");
  if (timef == NULL) {
    perror("time file fopen\n");
    assert(0);
  }

  dram_devdax_mmap = mmap(NULL, DRAMSIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, dramfd, 0);
  if (dram_devdax_mmap == MAP_FAILED) {
    perror("dram devdax mmap");
    assert(0);
  }

  nvm_devdax_mmap = mmap(NULL, NVMSIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, nvmfd, 0);
  if (nvm_devdax_mmap == MAP_FAILED) {
    perror("nvm devdax mmap");
    assert(0);
  }
  
  paging_init();
  
  init = true;
}


void* hemem_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
  void *p;
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


int hemem_munmap(void* addr, size_t length)
{
  return munmap(addr, length);
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


void hemem_migrate_up(struct hemem_page *page, uint64_t dram_offset)
{
  void *old_addr;
  void *new_addr;
  void *newptr;
  struct timeval migrate_start, migrate_end;
  struct timeval start, end;
  uint64_t old_addr_offset, new_addr_offset;

  gettimeofday(&migrate_start, NULL);
  
  assert(page != NULL);
  old_addr_offset = page->devdax_offset;
  new_addr_offset = dram_offset;

  old_addr = nvm_devdax_mmap + old_addr_offset;

  new_addr = dram_devdax_mmap + new_addr_offset;

  // copy page from faulting location to temp location
  gettimeofday(&start, NULL);
  memcpy(new_addr, old_addr, PAGE_SIZE);
  gettimeofday(&end, NULL);
  LOG_TIME("memcpy_to_dram: %f s\n", elapsed(&start, &end));

  gettimeofday(&start, NULL);
  newptr = mmap((void*)page->va, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE | MAP_FIXED, dramfd, new_addr_offset);
  if (newptr == MAP_FAILED) {
    perror("newptr mmap");
    assert(0);
  }
  if (newptr != (void*)page->va) {
    printf("mapped address is not same as faulting address\n");
  }
  gettimeofday(&end, NULL);
  LOG_TIME("mmap_dram: %f s\n", elapsed(&start, &end));

  // re-register new mmap region with userfaultfd
  gettimeofday(&start, NULL);
  struct uffdio_register uffdio_register;
  uffdio_register.range.start = (uint64_t)newptr;
  uffdio_register.range.len = PAGE_SIZE;
  uffdio_register.mode = UFFDIO_REGISTER_MODE_MISSING | UFFDIO_REGISTER_MODE_WP;
  uffdio_register.ioctls = 0;
  /* fprintf(stderr, "re-register start: 0x%llx\tend: 0x%llx\tlength: %lld\n", uffdio_register.range.start, uffdio_register.range.start + uffdio_register.range.len, uffdio_register.range.len); */
  if (ioctl(uffd, UFFDIO_REGISTER, &uffdio_register) == -1) {
    perror("ioctl uffdio_register");
    assert(0);
  }
  gettimeofday(&end, NULL);
  LOG_TIME("uffdio_register: %f s\n", elapsed(&start, &end));
  
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

  gettimeofday(&migrate_start, NULL);
  
  assert(page != NULL);
  old_addr_offset = page->devdax_offset;
  new_addr_offset = nvm_offset;

  old_addr = dram_devdax_mmap + old_addr_offset;

  new_addr = nvm_devdax_mmap + new_addr_offset;

  // copy page from faulting location to temp location
  gettimeofday(&start, NULL);
  memcpy(new_addr, old_addr, PAGE_SIZE);
  gettimeofday(&end, NULL);
  LOG_TIME("memcpy_to_nvm: %f s\n", elapsed(&start, &end));

  gettimeofday(&start, NULL);
  newptr = mmap((void*)page->va, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE | MAP_FIXED, nvmfd, new_addr_offset);
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
  /* fprintf(stderr, "re-register start: 0x%llx\tend: 0x%llx\tlength: %lld\n", uffdio_register.range.start, uffdio_register.range.start + uffdio_register.range.len, uffdio_register.range.len); */
  if (ioctl(uffd, UFFDIO_REGISTER, &uffdio_register) == -1) {
    perror("ioctl uffdio_register");
    assert(0);
  }
  gettimeofday(&end, NULL);
  LOG_TIME("uffdio_register: %f s\n", elapsed(&start, &end));
  
  gettimeofday(&migrate_end, NULL);  
  LOG_TIME("hemem_migrate_down: %f s\n", elapsed(&migrate_start, &migrate_end));
}

void hemem_wp_page(struct hemem_page *page, bool protect)
{
  uint64_t addr = page->va;
  struct uffdio_writeprotect wp;
  int ret;
  struct timeval start, end;
  
  LOG("Write protect va: 0x%lx\n", addr);

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
  LOG_TIME("uffdio_writeprotect: %f s\n", elapsed(&start, &end));
}


void handle_wp_fault(uint64_t page_boundry)
{
  //assert(!"wp fault handling not yet implemented\n");
  struct hemem_page *page;

  page = find_page(page_boundry);
  assert(page != NULL);

  pthread_mutex_lock(&(page->page_lock));

  assert(!page->migrating);

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

  gettimeofday(&missing_start, NULL);
  page = (struct hemem_page*)calloc(1, sizeof(struct hemem_page));
  if (page == NULL) {
    perror("page calloc");
    assert(0);
  }

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
  newptr = mmap((void*)page_boundry, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE | MAP_FIXED, (in_dram ? dramfd : nvmfd), offset);
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

  gettimeofday(&end, NULL);
  // re-register new mmap region with userfaultfd
  struct uffdio_register uffdio_register;
  uffdio_register.range.start = (uint64_t)newptr;
  uffdio_register.range.len = PAGE_SIZE;
  uffdio_register.mode = UFFDIO_REGISTER_MODE_MISSING | UFFDIO_REGISTER_MODE_WP;
  uffdio_register.ioctls = 0;
  /* fprintf(stderr, "re-register start: 0x%llx\tend: 0x%llx\tlength: %lld\n", uffdio_register.range.start, uffdio_register.range.start + uffdio_register.range.len, uffdio_register.range.len); */
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

  // place in hemem's page tracking list
  enqueue_page(page);

  gettimeofday(&end, NULL);
  missing_faults_handled++;
  gettimeofday(&missing_end, NULL);
  LOG_TIME("hemem_missing_fault: %f s\n", elapsed(&missing_start, &missing_end));
  //printf("page missing fault took %.4f seconds\n", elapsed(&start, &end));
}


void *handle_fault()
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


uint64_t hemem_va_to_pa(uint64_t va)
{
  uint64_t page_boundry = va & ~(PAGE_SIZE - 1);
  return va_to_pa(page_boundry);
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
