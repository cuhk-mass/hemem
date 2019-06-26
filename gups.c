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
#include <libpmem.h>
#include <libvmem.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <errno.h>

#define DRAMPATH "/dev/dax0.0"
#define NVMPATH "/dev/dax1.0"

#define HUGEPAGE_SIZE 2 * (1024 * 1024)

extern void calc_indices(unsigned long* indices, unsigned long updates, unsigned long nelems);

struct thread_data {
  unsigned long *indices;       // array of indices to access
  void *field;                  // pointer to start of thread's region
};

struct thread_data* td;

struct args {
  int tid;                      // thread id
  struct thread_data* td;       // thread data for thread
  unsigned long iters;          // iterations to perform
  unsigned long size;           // size of region
  unsigned long elt_size;       // size of elements
};

struct remap_args {
  long uffd;                    // userfault fd (for write protection)
  void* region;	                // ptr to region
  unsigned long region_size;    // size of region
  int dram_fd;                  // fd for dram devdax file
  int nvm_fd;                   // fd for nvm devdax file
  int base_pages;               // use base pages or not
  int nvm_to_dram;              // remap nvm to dram or dram to nvm
};

struct fault_args {
  long uffd;                    // userfault fd
  int dram_fd;                  // fd for dram devdax file
  int nvm_fd;                   // fd for nvm devdax file
  int nvm_to_dram;              // remap nvm to dram or dram to nvm
};

/* Returns the number of seconds encoded in T, a "struct timeval". */
#define tv_to_double(t) (t.tv_sec + (t.tv_usec / 1000000.0))

/* Useful for doing arithmetic on struct timevals. M*/
void
timeDiff(struct timeval *d, struct timeval *a, struct timeval *b)
{
  d->tv_sec = a->tv_sec - b->tv_sec;
  d->tv_usec = a->tv_usec - b->tv_usec;
  if (d->tv_usec < 0) {
    d->tv_sec -= 1;
    d->tv_usec += 1000000;
  }
}


/* Return the no. of elapsed seconds between Starttime and Endtime. */
double
elapsed(struct timeval *starttime, struct timeval *endtime)
{
  struct timeval diff;

  timeDiff(&diff, endtime, starttime);
  return tv_to_double(diff);
}

void 
*handle_fault(void* arg)
{
  static struct uffd_msg msg;
  struct fault_args *fa = (struct fault_args*)arg;
  long uffd = fa->uffd;
  int dramfd = fa->dram_fd;
  int nvmfd = fa->nvm_fd;
  int nvm_to_dram = fa->nvm_to_dram;
  ssize_t nread;
  unsigned long fault_addr;
  unsigned long fault_flags;
  unsigned long page_boundry;
  void* tmp_page;
  void* newptr;
  struct uffdio_range range;
  int ret;
  void* zero_page;

  printf("fault handler entered\n");

  if (nvm_to_dram) {
    zero_page = mmap(NULL, HUGEPAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, nvmfd, 0);
  }
  else {
    zero_page = mmap(NULL, HUGEPAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, dramfd, 0);
  }

  if (zero_page == MAP_FAILED) {
    perror("mmap zero page");
    assert(0);
  }

  memset(zero_page, 0, HUGEPAGE_SIZE);

  //TODO: handle write protection fault (if possible)
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
      continue;
    case 1:
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

    if (nread != sizeof(msg)) {
      printf("invalid msg size\n");
      assert(0);
    }

    //TODO: check page fault event, handle it
    if (msg.event & UFFD_EVENT_PAGEFAULT) {
      //TODO: handle missing page fault (will this ever happen?)
      //TODO: handle wp page fault -- how?
      fault_addr = (unsigned long)msg.arg.pagefault.address;
      fault_flags = msg.arg.pagefault.flags;

      // allign faulting address to page boundry
      // huge page boundry in this case due to dax allignment
      page_boundry = fault_addr & ~(HUGEPAGE_SIZE - 1);
      //printf("page boundry is 0x%lld\n", page_boundry);

      if (fault_flags & UFFD_PAGEFAULT_FLAG_WP) {
        // if it is a write protection falut, it is because the remapper thread
	// flagged the page as ready to move by write-protecting it, so this is
	// the case where we migrate the page
	printf("received a write-protection fault at addr 0x%lld\n", fault_addr);
	
        if (nvm_to_dram) {
          // map page from dram
          tmp_page = mmap(NULL, HUGEPAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, dramfd, 0);
	}
	else {
          tmp_page = mmap(NULL, HUGEPAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, nvmfd, 0);
	}

	if (tmp_page == MAP_FAILED) {
          perror("tmp page mmap");
	  assert(0);
	}

	// copy page from faulting location to temp location
	memcpy(tmp_page, (void*)page_boundry, HUGEPAGE_SIZE);

	if (nvm_to_dram) {
          newptr = mmap((void*)page_boundry, HUGEPAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE | MAP_FIXED, dramfd, 0);
        }
        else {
          newptr = mmap((void*)page_boundry, HUGEPAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE | MAP_FIXED, nvmfd, 0);
	}

	if (newptr == MAP_FAILED) {
          perror("mmap");
	  assert(0);
	}
	if (newptr != (void*)page_boundry) {
          printf("mapped address is not same as faulting address\n");
	}

	munmap(tmp_page, HUGEPAGE_SIZE);
      }
      else {
        // if the fault is one of the page missing cases, then this is our first access
        // to the page. Since we turn swapping off and (as far as I am aware) userfaultfd
        // prevents the OS from doing its own page management things to the region, it
        // can only be the first touch case. Mapping a zero page should then do the trick
 
/*       
        if (fault_flags & UFFD_PAGEFAULT_FLAG_WRITE) {
          printf("received a page missing write fault at addr 0x%lld\n", fault_addr);
        }
        else {
          printf("received a page missing read fault at addr 0x%lld\n", fault_addr);
        }
*/
	if (nvm_to_dram) {
          newptr = mmap((void*)page_boundry, HUGEPAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE | MAP_FIXED, nvmfd, 0);
        }
	else {
          newptr = mmap((void*)page_boundry, HUGEPAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE | MAP_FIXED, dramfd, 0);
	}

	if (newptr == MAP_FAILED) {
          perror("mmap");
	  assert(0);
	}
	if (newptr != (void*)page_boundry) {
          printf("mapped address is not same as faulting address\n");
	}

	memcpy(newptr, zero_page, HUGEPAGE_SIZE);

	munmap(tmp_page, HUGEPAGE_SIZE);
      }

      // wake the faulting thread
      range.start = fault_addr;
      range.len = HUGEPAGE_SIZE;

      ret = ioctl(uffd, UFFDIO_WAKE, &range);

      if (ret < 0) {
        perror("uffdio wake");
	assert(0);
      }
    }
  }
}


void 
*do_remap(void *args)
{
  //printf("do_remap entered\n");
  struct remap_args *re = (struct remap_args*)args;
  long uffd = re->uffd;
  void* field = re->region;
  unsigned long size = re->region_size;
  int dramfd = re->dram_fd;
  int nvmfd = re->nvm_fd;
  int base = re->base_pages;
  int nvm_to_dram = re->nvm_to_dram;
  void *ptr = NULL;
  struct timeval start, end;
  int ret = 0;
  void *newptr;
  struct uffdio_writeprotect wp;
  void* wp_ptr;

  assert(field != NULL);
  //printf("do_remap:\tfield: 0x%x\tfd: %d\tbase: %d\tsize: %llu\tnvm_to_dram: %d\n",field, fd, base, size, nvm_to_dram);

  // go through region hugepage-by-hugepage, marking as write protected
  // the fault handling thread will do the actual migration
  for (wp_ptr = field; wp_ptr < field + size; wp_ptr += HUGEPAGE_SIZE) {
    sleep(1);

    printf("Changing protection to read only\n");
    wp.range.start = (unsigned long)wp_ptr;
    wp.range.len = HUGEPAGE_SIZE;
    wp.mode = UFFDIO_WRITEPROTECT_MODE_WP;
    ret = ioctl(uffd, UFFDIO_WRITEPROTECT, &wp);
  
    if (ret < 0) {
      perror("uffdio writeprotect");
      assert(0);
    }

    printf("Protection changed\n");
  }

}


#define GET_NEXT_INDEX(tid, i, size) td[tid].indices[i]

void
*do_gups(void *arguments)
{
  //printf("do_gups entered\n");
  struct args *args = (struct args*)arguments;
  char *field = (char*)(args->td->field);
  unsigned long i;
  unsigned long long index;
  unsigned long elt_size = args->elt_size;
  char data[elt_size];

  for (i = 0; i < args->iters; i++) {
    index = GET_NEXT_INDEX(args->tid, i, args->size);
    memset(data, i, elt_size);
    memcpy(&field[index * elt_size], data, elt_size);
  }
}


int
main(int argc, char **argv)
{
  int threads;
  unsigned long updates, expt;
  unsigned long size, elt_size, nelems;
  struct timeval starttime, stoptime;
  double secs, gups;
  int dram = 0, base = 0;
  VMEM *vmp;
  int remap = 0;
  int dramfd = 0, nvmfd = 0;
  long uffd;
  struct uffdio_api uffdio_api;
  struct uffdio_register uffdio_register;

  struct timeval start, end;

  if (argc != 8) {
    printf("Usage: %s [threads] [updates per thread] [exponent] [data size (bytes)] [DRAM/NVM] [base/huge] [noremap/remap]\n", argv[0]);
    printf("  threads\t\t\tnumber of threads to launch\n");
    printf("  updates per thread\t\tnumber of updates per thread\n");
    printf("  exponent\t\t\tlog size of region\n");
    printf("  data size\t\t\tsize of data in array (in bytes)\n");
    printf("  DRAM/NVM\t\t\twhether the region is in DRAM or NVM\n");
    printf("  base/huge\t\t\twhether to map the region with base or huge pages\n");
    printf("  noremap/remap\t\t\twhether to remap the region when accessing\n");
    return 0;
  }

  threads = atoi(argv[1]);
  td = (struct thread_data*)malloc(threads * sizeof(struct thread_data)); 
  
  updates = atol(argv[2]);
  updates -= updates % 256;
  expt = atoi(argv[3]);
  assert(expt > 8);
  assert(updates > 0 && (updates % 256 == 0));
  size = (unsigned long)(1) << expt;
  size -= (size % 256);
  assert(size > 0 && (size % 256 == 0));
  elt_size = atoi(argv[4]);
 
  if ((vmp = vmem_create("/mnt/pmem12", (1024*1024*1024))) == NULL) {
    perror("vmem_create");
    exit(1);
  }
  
  if (!strcmp("DRAM", argv[5])) {
    dram = 1;
  }

  if (!strcmp("base", argv[6])) {
    base = 1;
  }

  if (!strcmp("remap", argv[7])) {
    remap = 1;
  }

  printf("%lu updates per thread (%d threads)\n", updates, threads);
  printf("field of 2^%lu (%lu) bytes\n", expt, size);
  printf("%d byte element size (%d elements total)\n", elt_size, size / elt_size);

  if (dram) {
    printf("Mapping in DRAM ");
  }
  else {
    printf("Mapping in NVM ");
  }

  if (base) {
    printf("with base pages\n");
  }
  else {
    printf("with huge pages\n");
  }

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
  //printf("DRAM fd: %d\tNVM fd: %d\n", dramfd, nvmfd);

  int i;
  void *p;
  if (dram) {
    // devdax doesn't like huge page flag
    gettimeofday(&starttime, NULL);
    p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, dramfd, 0);
  }
  else {
    gettimeofday(&starttime, NULL);
    // devdax doesn't like huge page flag
    p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, nvmfd, 0);
  }

  if (p == NULL || p == MAP_FAILED) {
    perror("mmap");
  }  
  assert(p != NULL && p != MAP_FAILED);

  gettimeofday(&stoptime, NULL);
  printf("Init mmap took %.4f seconds\n", elapsed(&starttime, &stoptime));
  printf("Region address: 0x%llx\t size: %lld\n", p, size);
  //printf("Field addr: 0x%x\n", p);

  nelems = (size / threads) / elt_size; // number of elements per thread

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

  uffdio_register.range.start = (unsigned long)p;
  uffdio_register.range.len = size;
  uffdio_register.mode = UFFDIO_REGISTER_MODE_MISSING;
  uffdio_register.ioctls = 0;
  if (ioctl(uffd, UFFDIO_REGISTER, &uffdio_register) == -1) {
    perror("ioctl uffdio_register");
    assert(0);
  }

  printf("Set up userfault success\n");

  pthread_t fault_thread;
  struct fault_args *fa = malloc(sizeof(struct fault_args));
  fa->uffd = uffd;
  fa->nvm_fd = nvmfd;
  fa->dram_fd = dramfd;
  fa->nvm_to_dram = !dram;
  int s = pthread_create(&fault_thread, NULL, handle_fault, (void*)fa);
  if (s != 0) {
    perror("pthread_create");
    assert(0);
  }

  printf("initializing thread data\n");
  gettimeofday(&starttime, NULL);
  for (i = 0; i < threads; i++) {
    td[i].field = p + (i * nelems * elt_size);
    //printf("thread %d start address: %llu\n", i, (unsigned long)td[i].field);
    td[i].indices = (unsigned long*)vmem_malloc(vmp, updates * sizeof(unsigned long));
    if (td[i].indices == NULL) {
      perror("vmem_malloc");
      exit(1);
    }
    calc_indices(td[i].indices, updates, nelems);
  }
  
  gettimeofday(&stoptime, NULL);
  secs = elapsed(&starttime, &stoptime);
  printf("Initialization time: %.4f seconds.\n", secs);

  printf("Timing.\n");
  pthread_t t[threads];
  pthread_t remap_thread;
  gettimeofday(&starttime, NULL);
  
  struct args **as = (struct args**)malloc(threads * sizeof(struct args*));
  // spawn gups worker threads
  for (i = 0; i < threads; i++) {
    as[i] = (struct args*)malloc(sizeof(struct args));
    as[i]->tid = i;
    as[i]->td = &td[i];
    as[i]->iters = updates;
    as[i]->size = nelems;
    as[i]->elt_size = elt_size;
    int r = pthread_create(&t[i], NULL, do_gups, (void*)as[i]);
    assert(r == 0);
  }

  // spawn remap thread (if remapping)
  if (remap) {
    struct remap_args *re = (struct remap_args*)malloc(sizeof(struct remap_args));
    re->uffd = uffd;
    re->region = p;
    re->dram_fd = dramfd;
    re->nvm_fd = nvmfd;
    re->base_pages = base;
    re->region_size = size;
    re->nvm_to_dram = !dram;
    //printf("field: 0x%x\tdramfd: %d\tnvmfd: %d\tbase: %d\tsize: %llu\tnvm_to_dram: %d\n", re->region, re->dram_fd, re->nvm_fd, re->base_pages, re->region_size, re->nvm_to_dram);
    int r = pthread_create(&remap_thread, NULL, do_remap, (void*)re);
    assert(r == 0);
  }

  // wait for worker threads
  for (i = 0; i < threads; i++) {
    int r = pthread_join(t[i], NULL);
    assert(r == 0);
  }

  // wait for remap thread (if remapping)
  if (remap) {
    int r = pthread_join(remap_thread, NULL);
    assert(r == 0);
  }
  gettimeofday(&stoptime, NULL);

  secs = elapsed(&starttime, &stoptime);
  printf("Elapsed time: %.4f seconds.\n", secs);
  gups = threads * ((double)updates) / (secs * 1.0e9);
  printf("GUPS = %.10f\n", gups);

  for (i = 0; i < threads; i++) {
    vmem_free(vmp, td[i].indices);
  }
  free(td);

  close(nvmfd);
  close(dramfd);
  munmap(p, size);

  return 0;
}
