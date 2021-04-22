/*
 * =====================================================================================
 *
 *       Filename:  gups.c
 *
 *    Description:
 *
 *        Version:  1.0
 *        Created:  02/21/2018 02:36:27 PM
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  YOUR NAME (),
 *   Organization:
 *
 * =====================================================================================
 */

#define _GNU_SOURCE

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

#include "../timer.h"

uint64_t size = 2 * 1024 * 1024;
uint64_t elt_size = 8;
uint64_t iters = 1000000000;

int dramfd, nvmfd, uffd;
void *dram_devdax_mmap, *nvm_devdax_mmap;

bool migrating = false;

void wp_page(uint64_t addr)
{
  struct uffdio_writeprotect wp;
  int ret;
 
  assert(addr != 0);
  
  wp.range.start = addr;
  wp.range.len = 2 * 1024 * 1024;
  wp.mode = UFFDIO_WRITEPROTECT_MODE_WP;
  ret = ioctl(uffd, UFFDIO_WRITEPROTECT, &wp);

  if (ret < 0) {
    perror("uffdio writeprotect");
    assert(0);
  }
}

void migrate(uint64_t va, bool migrate_down)
{
  void *newptr;
  uint64_t pagesize;
  void *old_addr, *new_addr;

  migrating = true;

  pagesize = 2 * 1024 * 1024;

  if (migrate_down) {
    old_addr = dram_devdax_mmap;
    new_addr = nvm_devdax_mmap;
  }
  else {
    old_addr = nvm_devdax_mmap;
    new_addr = dram_devdax_mmap;
  }
  
  memcpy(new_addr, old_addr, pagesize);

  if (migrate_down) {
    newptr = mmap((void*)va, pagesize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE | MAP_FIXED, nvmfd, 0);
  }
  else {
    newptr = mmap((void*)va, pagesize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE | MAP_FIXED, dramfd, 0);
  }

  if (newptr == MAP_FAILED) {
    perror("newptr mmap");
    assert(0);
  }
  if (newptr != (void*)va) {
    fprintf(stderr, "mapped address is not same as faulting address\n");
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

  migrating = false;
}

void *do_migration(void *addr)
{
  for (;;) {
    wp_page((uint64_t)addr);
    migrate((uint64_t)addr, true);
    //usleep(50000);
    
    wp_page((uint64_t)addr);
    migrate((uint64_t)addr, false);
    //usleep(50000);
  }

  return NULL;
}

void handle_wp_fault(uint64_t page_boundry)
{
  fprintf(stderr, "encountered a wp fault, waiting for migration...");
  while (migrating) {
    // just wait for migrating to be done
  }
  fprintf(stderr, "done\n");
}

void *handle_fault()
{
  static struct uffd_msg *msg;
  ssize_t nread;
  uint64_t fault_addr;
  uint64_t fault_flags;
  uint64_t page_boundry;
  struct uffdio_range range;
  int ret;

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

    nread = read(uffd, msg, sizeof(struct uffd_msg));
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

    if ((nread != sizeof(struct uffd_msg))) {
      fprintf(stderr, "invalid msg size: [%ld]\n", nread);
      assert(0);
    }

    if (msg->event & UFFD_EVENT_PAGEFAULT) {
      fault_addr = (uint64_t)msg->arg.pagefault.address;
      fault_flags = msg->arg.pagefault.flags;

      // allign faulting address to page boundry
      // huge page boundry in this case due to dax allignment
      page_boundry = fault_addr & ~((2 * 1024 * 1024) - 1);

      if (fault_flags & UFFD_PAGEFAULT_FLAG_WP) {
        handle_wp_fault(page_boundry);
      }
      else {
        assert(!"page faults with MAP_POPULATE should not happen\n");
      }

      // wake the faulting thread
      range.start = (uint64_t)page_boundry;
      range.len = 2 * 1024 * 1024;

      ret = ioctl(uffd, UFFDIO_WAKE, &range);

      if (ret < 0) {
        perror("uffdio wake");
        assert(0);
      }
    }
    else if (msg->event & UFFD_EVENT_UNMAP){
      fprintf(stderr, "Received an unmap event\n");
      assert(0);
    }
    else if (msg->event & UFFD_EVENT_REMOVE) {
      fprintf(stderr, "received a remove event\n");
      assert(0);
    }
    else {
      fprintf(stderr, "received a non page fault event\n");
      assert(0);
    }
  }
}


static uint64_t lfsr_fast(uint64_t lfsr)
{
  lfsr ^= lfsr >> 7;
  lfsr ^= lfsr << 9;
  lfsr ^= lfsr >> 13;
  return lfsr;
}

static void *do_gups(void *argument)
{
  //printf("do_gups entered\n");
  char *field = (char*)(argument);
  uint64_t i;
  uint64_t index;
  char data[elt_size];
  uint64_t lfsr;
  
  srand(0);
  lfsr = rand();

  for (i = 0; i < iters; i++) {
    lfsr = lfsr_fast(lfsr);
    index = lfsr % (size / elt_size);
    memcpy(data, &field[index * elt_size], elt_size);
    memset(data, data[0] + i, elt_size);
  }
  return 0;
}

int main(int argc, char **argv)
{
  struct timeval starttime, stoptime;
  double secs, gups;
  void *p;
  uint64_t nelems;
  pthread_t gups_thread, fault_thread, migrate_thread;

  uffd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
  if (uffd == -1) {
    perror("uffd");
    assert(0);
  }

  struct uffdio_api uffdio_api;
  uffdio_api.api = UFFD_API;
  uffdio_api.features = UFFD_FEATURE_PAGEFAULT_FLAG_WP |  UFFD_FEATURE_MISSING_SHMEM | UFFD_FEATURE_MISSING_HUGETLBFS;// | UFFD_FEATURE_EVENT_UNMAP | UFFD_FEATURE_EVENT_REMOVE;
  uffdio_api.ioctls = 0;
  if (ioctl(uffd, UFFDIO_API, &uffdio_api) == -1) {
    perror("ioctl uffdio_api");
    assert(0);
  }

  dramfd = open("/dev/dax0.0", O_RDWR);
  if (dramfd < 0) {
    perror("dram open");
  }
  assert(dramfd >= 0);

  nvmfd = open("/dev/dax1.0", O_RDWR);
  if (nvmfd < 0) {
    perror("nvm open");
  }
  assert(nvmfd >= 0);

  dram_devdax_mmap = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, dramfd, 0);
  if (dram_devdax_mmap == MAP_FAILED) {
    perror("dram devdax mmap");
    assert(0);
  }

  nvm_devdax_mmap = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, nvmfd, 0);
  if (nvm_devdax_mmap == MAP_FAILED) {
    perror("nvm devdax mmap");
    assert(0);
  }

  gettimeofday(&starttime, NULL);

  fprintf(stderr, "%ld updates per thread (1 threads)\n", iters);
  fprintf(stderr, "field of 2^21 (%lu) bytes\n", size);
  fprintf(stderr, "8 byte element size (%ld elements total)\n", size / elt_size);

  p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, dramfd, 0);
  if (p == MAP_FAILED) {
    perror("mmap");
    assert(0);
  }

  struct uffdio_register uffdio_register;
  uffdio_register.range.start = (uint64_t)p;
  uffdio_register.range.len =  size;
  uffdio_register.mode = UFFDIO_REGISTER_MODE_MISSING | UFFDIO_REGISTER_MODE_WP;
  uffdio_register.ioctls = 0;
  if (ioctl(uffd, UFFDIO_REGISTER, &uffdio_register) == -1) {
    perror("ioctl uffdio_register");
    assert(0);
  }

  int r = pthread_create(&fault_thread, NULL, handle_fault, NULL);
  assert(r == 0);

  r = pthread_create(&migrate_thread, NULL, do_migration, p);
  assert(r == 0);

  gettimeofday(&stoptime, NULL);
  fprintf(stderr, "Init took %.4f seconds\n", elapsed(&starttime, &stoptime));
  fprintf(stderr, "Region address: %p\t size: %ld\n", p, size);

  nelems = size / elt_size; // number of elements per thread
  fprintf(stderr, "Elements per thread: %lu\n", nelems);

  gettimeofday(&starttime, NULL);

  // run through gups once to touch all memory
  // spawn gups worker thread
  r = pthread_create(&gups_thread, NULL, do_gups, (void*)p);
  assert(r == 0);

  r = pthread_join(gups_thread, NULL);
  assert(r == 0);

  gettimeofday(&stoptime, NULL);

  secs = elapsed(&starttime, &stoptime);
  printf("Elapsed time: %.4f seconds.\n", secs);
  gups = ((double)iters) / (secs * 1.0e9);
  printf("GUPS = %.10f\n", gups);

  fprintf(stderr, "Timing.\n");
  gettimeofday(&starttime, NULL);
  
  r = pthread_create(&gups_thread, NULL, do_gups, (void*)p);
  assert(r == 0);

  r = pthread_join(gups_thread, NULL);
  assert(r == 0);
  
  gettimeofday(&stoptime, NULL);

  secs = elapsed(&starttime, &stoptime);
  printf("Elapsed time: %.4f seconds.\n", secs);
  gups = ((double)iters) / (secs * 1.0e9);
  printf("GUPS = %.10f\n", gups);

  printf("Timing.\n");
  gettimeofday(&starttime, NULL);

  // spawn gups worker threads
  r = pthread_create(&gups_thread, NULL, do_gups, (void*)p);
  assert(r == 0);

  r = pthread_join(gups_thread, NULL);
  assert(r == 0);

  gettimeofday(&stoptime, NULL);

  secs = elapsed(&starttime, &stoptime);
  printf("Elapsed time: %.4f seconds.\n", secs);
  gups = ((double)iters) / (secs * 1.0e9);
  printf("GUPS = %.10f\n", gups);
  
  munmap(p, size);

  return 0;
}

