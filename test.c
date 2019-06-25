#include <sys/types.h>
#include <linux/userfaultfd.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <pthread.h>
#define _GNU_SOURCE         /* See feature_test_macros(7) */
#include <unistd.h>
#include <sys/syscall.h>   /* For SYS_xxx definitions */
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

//#define MMAP_LEN	4096
#define MMAP_LEN	(2 * 1024 * 1024)

static int dfd = -1;
static void *addr = NULL;

static void *fault_handler_thread(void *arg)
{
  static struct uffd_msg msg;   /* Data read from userfaultfd */
  long uffd = (long)arg;        /* userfaultfd file descriptor */
  ssize_t nread;
  struct uffdio_register uffdio_register;

  sleep(4);
  
  // Move to DRAM -- Unmap
  int r = munmap(addr, MMAP_LEN);
  assert(r == 0);
  addr = mmap(addr, MMAP_LEN, PROT_READ | PROT_WRITE,
	      MAP_SHARED | MAP_FIXED, dfd, 0);
  if(addr == MAP_FAILED) {
    perror("mmap");
  }
  assert(addr != MAP_FAILED);

  // Register userfaultfd
  uffdio_register.range.start = (unsigned long) addr;
  uffdio_register.range.len = MMAP_LEN;
  /* uffdio_register.mode = UFFDIO_REGISTER_MODE_MISSING | UFFDIO_REGISTER_MODE_WP; */
  uffdio_register.mode = UFFDIO_REGISTER_MODE_MISSING;
  r = ioctl(uffd, UFFDIO_REGISTER, &uffdio_register);
  if(r == -1) {
    perror("test");
  }
  assert(r != -1);
  
  for (;;) {
    /* Read an event from the userfaultfd */
    nread = read(uffd, &msg, sizeof(msg));
    if (nread == 0) {
      printf("EOF on userfaultfd!\n");
      exit(EXIT_FAILURE);
    }

    assert(nread != -1);

    /* We expect only one kind of event; verify that assumption */
    if (msg.event != UFFD_EVENT_PAGEFAULT) {
      fprintf(stderr, "Unexpected event on userfaultfd\n");
      exit(EXIT_FAILURE);
    }

    /* Display info about the page-fault event */
    printf("    UFFD_EVENT_PAGEFAULT event: ");
    printf("flags = %llx; ", msg.arg.pagefault.flags);
    printf("address = %llx\n", msg.arg.pagefault.address);
    void *addr = (void *)msg.arg.pagefault.address;

    // Map the NVM page read-only for copying
    void *oldaddr = mmap(NULL, MMAP_LEN, PROT_READ,
    		MAP_SHARED | MAP_POPULATE, dfd, 0);
    assert(oldaddr != MAP_FAILED);

    // Map some DRAM
    void *newaddr = mmap(NULL, MMAP_LEN, PROT_READ | PROT_WRITE,
    		MAP_SHARED | MAP_POPULATE, dfd, MMAP_LEN);
    assert(newaddr != MAP_FAILED);

    // Copy from NVM to DRAM
    memcpy(newaddr, oldaddr, MMAP_LEN);

    r = munmap(newaddr, MMAP_LEN);
    assert(r == 0);
    r = munmap(oldaddr, MMAP_LEN);
    assert(r == 0);
    
    // Overmap the faulting page -- atomic?
    addr = mmap(addr, MMAP_LEN, PROT_READ | PROT_WRITE,
		MAP_SHARED | MAP_POPULATE | MAP_FIXED, dfd, MMAP_LEN);
    assert(addr != MAP_FAILED);
    
    // Continue the faulting thread
    struct uffdio_range range = {
      .start = msg.arg.pagefault.address,
      .len = MMAP_LEN,
    };
    r = ioctl(uffd, UFFDIO_WAKE, &range);
    assert(r == 0);
  }

  return NULL;
}

int main(int argc, char *argv[])
{
  struct uffdio_api uffdio_api;

  long fd = syscall(__NR_userfaultfd, 0);
  assert(fd != -1);
  /* printf("fd = %ld\n", fd); */

  uffdio_api.api = UFFD_API;
  /* uffdio_api.features = UFFD_FEATURE_MISSING_HUGETLBFS | UFFD_FEATURE_MISSING_SHMEM; */
  uffdio_api.features = 0;
  int r = ioctl(fd, UFFDIO_API, &uffdio_api);
  assert(r != -1);

  /* void *addr = mmap(NULL, MMAP_LEN, PROT_READ | PROT_WRITE, */
  /* 		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); */
  dfd = open("/dev/dax0.0", O_RDWR);
  /* dfd = open("/dev/zero", O_RDWR); */
  assert(dfd != -1);
  addr = mmap(NULL, MMAP_LEN, PROT_READ | PROT_WRITE,
	      MAP_SHARED | MAP_POPULATE, dfd, 0);
  if(addr == MAP_FAILED) {
    perror("mmap");
  }
  assert(addr != MAP_FAILED);

  printf("Address returned by mmap() = %p\n", addr);

  // Fill NVM
  char *p = addr;
  p[0] = 1;
  
  pthread_t t;
  r = pthread_create(&t, NULL, fault_handler_thread, (void *)fd);
  assert(r == 0);

  /* r = mprotect(addr, MMAP_LEN, PROT_READ); */
  /* assert(r == 0); */
  for(int i = 0; i < 20; i++) {
    sleep(1);
    printf("p = %d\n", p[0]);
    p[0]++;
  }

  return 0;
}
