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

#include "hemem.h"
#include "timer.h"

pthread_t fault_thread;

int dramfd = -1;
int nvmfd = -1;
long uffd = -1;
int init = 0;
unsigned long mem_allocated = 0;
int wp_faults_handled = 0;
int missing_faults_handled = 0;

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


  //close(nvmfd);
  //close(dramfd);

  init = 1;
}


void* 
hemem_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
  void* p;
  
  assert(init);

  // reserve block of memory
  p = mmap(addr, length, prot, MAP_SHARED, dramfd, offset);
  if (p == NULL || p == MAP_FAILED) {
    perror("mmap");
  }  
  assert(p != NULL && p != MAP_FAILED);

  unsigned long num_pages = (length / PAGE_SIZE);
  printf("number of pages in region: %lu\n", num_pages);

  // register with uffd
  struct uffdio_register uffdio_register;
  uffdio_register.range.start = (unsigned long)p;
  uffdio_register.range.len = length;
  uffdio_register.mode = UFFDIO_REGISTER_MODE_MISSING | UFFDIO_REGISTER_MODE_WP;
  uffdio_register.ioctls = 0;
  if (ioctl(uffd, UFFDIO_REGISTER, &uffdio_register) == -1) {
    perror("ioctl uffdio_register");
    assert(0);
  }
  
  // register with uffd page-by-page
/*
  for (register_ptr = p; register_ptr < p + length; register_ptr += PAGE_SIZE) {
    struct uffdio_register uffdio_register;
    uffdio_register.range.start = (unsigned long)register_ptr;
    uffdio_register.range.len = PAGE_SIZE;
    uffdio_register.mode = UFFDIO_REGISTER_MODE_MISSING | UFFDIO_REGISTER_MODE_WP;
    uffdio_register.ioctls = 0;
    if (!(((unsigned long long)(register_ptr) & ((unsigned long long)(2 * 1024 * 1024) - 1)) == 0)) {
      printf("not aligned: %p\n", register_ptr);
    }
    else {
      printf("aligned: %p\n", register_ptr);
    }
    
    printf("start: %llx\tend: %llx\tlen:%llu\n", uffdio_register.range.start, uffdio_register.range.start + uffdio_register.range.len, uffdio_register.range.len);
    if (ioctl(uffd, UFFDIO_REGISTER, &uffdio_register) == -1) {
      perror("ioctl uffdio_register");
      assert(0);
    }
    //printf("registered region: 0x%llx - 0x%llx\n", register_ptr, register_ptr + PAGE_SIZE);
    page++;
  }

  printf("registered %d pages with userfaultfd\n", page);
*/
  
  printf("Set up userfault success\n");

  return p;
}


int
hemem_munmap(void* addr, size_t length)
{
  return munmap(addr, length);
}


void
handle_wp_fault(unsigned long page_boundry, void* field)
{
  void* old_addr;
  void* new_addr;
  void* newptr;
  struct timeval start, end;
  int nvm_to_dram = 0;

  gettimeofday(&start, NULL);

  // map virtual address to dax file offset
  unsigned long offset = page_boundry - (unsigned long)field;
  //printf("page boundry: 0x%llx\tcalculated offset in dax file: 0x%llx\n", page_boundry, offset);

  //TODO: figure out how to tell whether block needs to move from NVM to DRAM or vice-versa
  //TODO: figure out how to keep track of mapping of virtual addresses -> /dev/dax file offsets
  if (nvm_to_dram) {
    old_addr = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, nvmfd, offset);
    new_addr = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, dramfd, offset);
  }
  else {
    old_addr = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, dramfd, offset);
    new_addr = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, nvmfd, offset);
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
    newptr = mmap((void*)page_boundry, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE | MAP_FIXED, dramfd, offset);
  }
  else {
    newptr = mmap((void*)page_boundry, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE | MAP_FIXED, nvmfd, offset);
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

/*
  if (wp_faults_handled % 1000 == 0) {
    printf("write protection fault took %.6f seconds\n", elapsed(&start, &end));
  }
*/
}


void
handle_missing_fault(unsigned long page_boundry)
{
  // Page mising fault case - probably the first touch case
  // allocate in DRAM as per LRU
  void* newptr;
  struct timeval start, end;

  // map virtual address to dax file offset
  unsigned long offset = (mem_allocated < DRAMSIZE) ? mem_allocated : mem_allocated - DRAMSIZE;
  //printf("page boundry: 0x%llx\tcalculated offset in dax file: 0x%llx\n", page_boundry, offset);

  gettimeofday(&start, NULL);

  if (mem_allocated < DRAMSIZE) {
    printf("allocating a page in DRAM\n");
    newptr = mmap((void*)page_boundry, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE | MAP_FIXED, dramfd, offset);
  }
  else {
    printf("allocating a page in NVM\n");
    newptr = mmap((void*)page_boundry, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE | MAP_FIXED, nvmfd, offset);
  }
  mem_allocated += PAGE_SIZE;

  if (newptr == NULL || newptr == MAP_FAILED) {
    perror("newptr mmap:");
    assert(0);
  }

  gettimeofday(&end, NULL);

  missing_faults_handled++;
  //printf("page missing fault took %.4f seconds\n", elapsed(&start, &end));
}


void 
*handle_fault(void* arg)
{
  static struct uffd_msg msg;
  void* field = arg;
  ssize_t nread;
  unsigned long fault_addr;
  unsigned long fault_flags;
  unsigned long page_boundry;
  struct uffdio_range range;
  int ret;

  printf("fault handler entered\n");

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
      printf("received a page fault event\n");
      fault_addr = (unsigned long)msg.arg.pagefault.address;
      fault_flags = msg.arg.pagefault.flags;

      // allign faulting address to page boundry
      // huge page boundry in this case due to dax allignment
      page_boundry = fault_addr & ~(PAGE_SIZE - 1);
      printf("page boundry is 0x%lx\n", page_boundry);

      if (fault_flags & UFFD_PAGEFAULT_FLAG_WP) {
        printf("received a write-protection fault at addr 0x%lx\n", fault_addr);
        handle_wp_fault(page_boundry, field);
      }
      else {
        printf("received a page missing fault at addr 0x%lx\n", fault_addr);
        handle_missing_fault(page_boundry);
      }

      //printf("waking thread\n");
      // wake the faulting thread
      range.start = (unsigned long)page_boundry;
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

#ifdef EXAMINE_PGTABLES
void
*examine_pagetables()
{
  FILE *maps;
  int pagemaps;
  FILE *kpageflags;
  char *line = NULL;
  ssize_t nread;
  size_t len;
  unsigned long vm_start, vm_end;
  int n, num_pages;
  long index;
  off64_t o;
  ssize_t t;
  struct pagemapEntry entry;

  maps = fopen("/proc/self/maps", "r");
  if (maps == NULL) {
    perror("/proc/self/maps fopen");
    assert(0);
  }

  pagemaps = open("/proc/self/pagemap", O_RDONLY);
  if (pagemaps == -1) {
    perror("/proc/self/pagemap fopen");
    assert(0);
  }

  kpageflags = fopen("/proc/kpageflags", "r");
  if (kpageflags == NULL) {
    perror("/proc/kpageflags fopen");
    assert(0);
  }

  nread = getline(&line, &len, maps); 
  while (nread != -1) {
    if (strstr(line, DRAMPATH) != NULL) {
      //printf("%s", line);
      n = sscanf(line, "%lX-%lX", &vm_start, &vm_end);
      if (n != 2) {
        printf("error, invalid line: %s\n", line);
        assert(0);
      }

      //printf("vm_start: %lX\tvm_end: %lX\n", vm_start, vm_end);
      num_pages = (vm_end - vm_start) / PAGE_SIZE;
      if (num_pages > 0) {
        index = (vm_start / PAGE_SIZE) * sizeof(unsigned long long);

        o = lseek64(pagemaps, index, SEEK_SET);

        if (o != index) {
          perror("pagemaps lseek");
          assert(0);
        }

        printf("num_pages: %d\n", num_pages);

        while (num_pages > 0) {
          unsigned long long pfn;
          t = read(pagemaps, &pfn, sizeof(unsigned long long));
          if (t < 0) {
            perror("pagemaps read");
            assert(0);
          }

          //printf("%016llX\n", pfn);
          entry.pfn = pfn & 0x7ffffffffffff;
          entry.soft_dirty = (pfn >> 55) & 1;
          entry.exclusive = (pfn >> 56) & 1;
          entry.file_page = (pfn >> 61) & 1;
          entry.swapped = (pfn >> 62) & 1;
          entry.present = (pfn >> 63) & 1;

          printf("%016llX\n", (entry.pfn * sysconf(_SC_PAGESIZE))); 
          num_pages--;
        }
      }
    }
    if (strstr(line, NVMPATH) != NULL) {
      n = sscanf(line, "%lX-%lX", &vm_start, &vm_end);
      if (n != 2) {
        printf("error, invalid line: %s\n", line);
        assert(0);
      }

      num_pages = (vm_end - vm_start) / PAGE_SIZE;
      if (num_pages > 0) {
        index = (vm_start / PAGE_SIZE) * sizeof(unsigned long long);

        o = lseek64(pagemaps, index, SEEK_SET);
        if (o != index) {
          perror("pagemaps lseek");
          assert(0);
        }

        printf("num_pages: %d\n", num_pages);

        while (num_pages > 0) {
          unsigned long long pfn;
          t = read(pagemaps, &pfn, sizeof(unsigned long long));
          if (t < 0) {
            perror("pagemaps read");
            assert(0);
          }

          //printf("%016llX\n", pfn);
          entry.pfn = pfn & 0x7ffffffffffff;
          entry.soft_dirty = (pfn >> 55) & 1;
          entry.exclusive = (pfn >> 56) & 1;
          entry.file_page = (pfn >> 61) & 1;
          entry.swapped = (pfn >> 62) & 1;
          entry.present = (pfn >> 63) & 1;

          printf("%016llX\n", (entry.pfn * sysconf(_SC_PAGE_SIZE)));
          num_pages--;
        }
      }    
      //printf("%s", line);
    }
    nread = getline(&line, &len, maps);
  }

  fclose(maps);
  close(pagemaps);
  fclose(kpageflags);
}
#endif
