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
#include <linux/userfaultfd.h>
#include <poll.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <errno.h>


#define EXAMINE_PGTABLES

#define DRAMPATH "/dev/dax0.0"
#define NVMPATH "/dev/dax1.1"

//#define HUGEPAGE_SIZE (1024 * 1024 * 1024)
#define HUGEPAGE_SIZE (2 * (1024 * 1024))
//#define HUGEPAGE_SIZE (4 * 1024)

extern void calc_indices(unsigned long* indices, unsigned long updates, unsigned long nelems);

struct gups_args {
  int tid;                      // thread id
  unsigned long *indices;       // array of indices to access
  void* field;                  // pointer to start of thread's region
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
  void* region;                 // ptr to region
  unsigned long size;           // size of region
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
  void* field = fa->region;
  unsigned long size = fa->size; 
  ssize_t nread;
  unsigned long fault_addr;
  unsigned long fault_flags;
  unsigned long page_boundry;
  void* old_addr;
  void* new_addr;
  void* newptr;
  struct uffdio_range range;
  int ret;
  struct timeval start, end;

  printf("fault handler entered\n");

  //TODO: handle write protection fault (if possible)
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
      fault_addr = (unsigned long)msg.arg.pagefault.address;
      fault_flags = msg.arg.pagefault.flags;

      // allign faulting address to page boundry
      // huge page boundry in this case due to dax allignment
      page_boundry = fault_addr & ~(HUGEPAGE_SIZE - 1);
      //printf("page boundry is 0x%lld\n", page_boundry);

      if (fault_flags & UFFD_PAGEFAULT_FLAG_WP) {
	//printf("received a write-protection fault at addr 0x%llx\n", fault_addr);
        
	gettimeofday(&start, NULL);

 
	// map virtual address to dax file offset
	unsigned long offset = page_boundry - (unsigned long)field;
	//printf("page boundry: 0x%llx\tcalculated offset in dax file: 0x%llx\n", page_boundry, offset);

	if (nvm_to_dram) {
          old_addr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, nvmfd, offset);
          new_addr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, dramfd, offset);
	}
	else {
          old_addr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, dramfd, offset);
          new_addr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, nvmfd, offset);
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
	memcpy(new_addr, old_addr, size);

	if (nvm_to_dram) {
          newptr = mmap((void*)page_boundry, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE | MAP_FIXED, dramfd, offset);
        }
        else {
          newptr = mmap((void*)page_boundry, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE | MAP_FIXED, nvmfd, offset);
	}

	if (newptr == MAP_FAILED) {
          perror("newptr mmap");
	  assert(0);
	}
	if (newptr != (void*)page_boundry) {
          printf("mapped address is not same as faulting address\n");
	}

	munmap(old_addr, size);
	munmap(new_addr, size);

	gettimeofday(&end, NULL);

        //printf("write protection fault took %.4f seconds\n", elapsed(&start, &end));

      }
      else {
	// Write protection faults seem to be passed as missing faults instead of
	// WP faults. This should be okay as we shouldn't really get page missing
	// faults anyway since the dax files shouldn't be swapped by the OS, and if
	// we map the original reagion with the MAP_POPULATE flag, then we won't
	// have first touch misses either. Thus, we assume any page missing fault
	// we receive is due to write protection
        //printf("received a page missing fault at addr 0x%llx\n", fault_addr);

	// map virtual address to dax file offset
	unsigned long offset = page_boundry - (unsigned long)field;
	//printf("page boundry: 0x%llx\tcalculated offset in dax file: 0x%llx\n", page_boundry, offset);

        gettimeofday(&start, NULL);
        
	if (nvm_to_dram) {
          old_addr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, nvmfd, offset);
          new_addr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, dramfd, offset);
	}
	else {
          old_addr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, dramfd, offset);
          new_addr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, nvmfd, offset);
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
	memcpy(new_addr, old_addr, size);

	if (nvm_to_dram) {
          newptr = mmap((void*)page_boundry, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE | MAP_FIXED, dramfd, offset);
        }
        else {
          newptr = mmap((void*)page_boundry, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE | MAP_FIXED, nvmfd, offset);
	}

	if (newptr == MAP_FAILED) {
          perror("newptr mmap");
	  assert(0);
	}
	if (newptr != (void*)page_boundry) {
          printf("mapped address is not same as faulting address\n");
	}

	munmap(old_addr, size);
	munmap(new_addr, size);

	gettimeofday(&end, NULL);

        printf("page missing fault took %.4f seconds\n", elapsed(&start, &end));
      }

      //printf("waking thread\n");
      // wake the faulting thread
      range.start = (unsigned long)page_boundry;
      range.len = size;

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
  void* wp_ptr;
  int wp_count = 0;

  assert(field != NULL);

  sleep(3);
  printf("Begin write protecting pages\n");
  gettimeofday(&start, NULL);

  //printf("do_remap:\tfield: 0x%x\tfd: %d\tbase: %d\tsize: %llu\tnvm_to_dram: %d\n",field, fd, base, size, nvm_to_dram);

  // go through region hugepage-by-hugepage, marking as write protected
  // the fault handling thread will do the actual migration
  int num_pages = size / (unsigned long)HUGEPAGE_SIZE;
  printf("write protecting %d pages\n", num_pages);

  for (wp_ptr = field; wp_ptr < field + size; wp_ptr += HUGEPAGE_SIZE) {
    //printf("Changing protection to read only on range 0x%llx - 0x%llx\n", wp_ptr, wp_ptr + HUGEPAGE_SIZE);
    struct uffdio_writeprotect wp;
    wp.range.start = (unsigned long)wp_ptr;
    wp.range.len = HUGEPAGE_SIZE;
    wp.mode = UFFDIO_WRITEPROTECT_MODE_WP;
    ret = ioctl(uffd, UFFDIO_WRITEPROTECT, &wp);
  
    if (ret < 0) {
      perror("uffdio writeprotect");
      assert(0);
    }
    //printf("Protection changed\n");

    wp_count++;
  }
  
  gettimeofday(&end, NULL);

  printf("Finished write protecting pages\n");
  printf("write protected %d pages in %.4f seconds\n", wp_count, elapsed(&start, &end));

}


void
*do_gups(void *arguments)
{
  //printf("do_gups entered\n");
  struct gups_args *args = (struct gups_args*)arguments;
  char *field = (char*)(args->field);
  unsigned long i;
  unsigned long long index;
  unsigned long elt_size = args->elt_size;
  char data[elt_size];

  //printf("Thread [%d] starting: field: [%llx]\n", args->tid, field);
  for (i = 0; i < args->iters; i++) {
    index = args->indices[i];
    memcpy(data, &field[index * elt_size], elt_size);
    memset(data, data[0] + i, elt_size);
    memcpy(&field[index * elt_size], data, elt_size);
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
      n = sscanf(line, "%lX-%lX", &vm_start, &vm_end);
      if (n != 2) {
        printf("error, invalid line: %s\n", line);
	assert(0);
      }

      num_pages = (vm_end - vm_start) / HUGEPAGE_SIZE;
      if (num_pages > 0) {
        index = (vm_start / HUGEPAGE_SIZE) * sizeof(unsigned long long);

        o = lseek64(pagemaps, index, SEEK_SET);

	if (o != index) {
          perror("pagemaps lseek");
	  assert(0);
	}

	printf("num_pages: %d\n", num_pages);

	while (num_pages > 0) {
          unsigned long long pa;

	  t = read(pagemaps, &pa, sizeof(unsigned long long));
	  if (t < 0) {
            perror("pagemaps read");
	    assert(0);
	  }

	  printf("%016llX\n", pa);
	  
	  num_pages--;
	}
      }
    }
    if (strstr(line, NVMPATH) != NULL) {
      printf("%s", line);
    }
    nread = getline(&line, &len, maps);
  }

  fclose(maps);
  close(pagemaps);
  fclose(kpageflags);
}
#endif

int
main(int argc, char **argv)
{
  int threads;
  unsigned long updates, expt;
  unsigned long size, elt_size, nelems;
  struct timeval starttime, stoptime;
  double secs, gups;
  int dram = 0, base = 0;
  int remap = 0;
  int dramfd = 0, nvmfd = 0;
  long uffd;
  struct uffdio_api uffdio_api;
  struct timeval start, end;
  int i;
  void *p, *register_ptr;
  struct gups_args** ga;

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

  gettimeofday(&starttime, NULL);
  
  threads = atoi(argv[1]);
  ga = (struct gups_args**)malloc(threads * sizeof(struct gups_args*));
  
  updates = atol(argv[2]);
  updates -= updates % 256;
  expt = atoi(argv[3]);
  assert(expt > 8);
  assert(updates > 0 && (updates % 256 == 0));
  size = (unsigned long)(1) << expt;
  size -= (size % 256);
  assert(size > 0 && (size % 256 == 0));
  elt_size = atoi(argv[4]);
  
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
  //printf("DRAM fd: %d\nNVM fd: %d\n", dramfd, nvmfd);

  if (dram) {
    // devdax doesn't like huge page flag
    gettimeofday(&starttime, NULL);
    p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, dramfd, 0);
  }
  else {
    gettimeofday(&starttime, NULL);
    // devdax doesn't like huge page flag
    p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, nvmfd, 0);
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
  printf("Elements per thread: %llu\n", nelems);

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
 
  unsigned long num_pages = (size / HUGEPAGE_SIZE);
  int page = 0;
  printf("number of pages in region: %llu\n", num_pages);

  for (register_ptr = p; register_ptr < p + size; register_ptr += HUGEPAGE_SIZE) {
    struct uffdio_register uffdio_register;
    uffdio_register.range.start = (unsigned long)register_ptr;
    uffdio_register.range.len = HUGEPAGE_SIZE;
    uffdio_register.mode = UFFDIO_REGISTER_MODE_MISSING | UFFDIO_REGISTER_MODE_WP;
    uffdio_register.ioctls = 0;
    if (ioctl(uffd, UFFDIO_REGISTER, &uffdio_register) == -1) {
      perror("ioctl uffdio_register");
      assert(0);
    }
    //printf("registered region: 0x%llx - 0x%llx\n", register_ptr, register_ptr + HUGEPAGE_SIZE);
    page++;
  }

  printf("registered %d pages with userfaultfd\n", page);
  printf("Set up userfault success\n");

  pthread_t fault_thread;
  struct fault_args *fa = malloc(sizeof(struct fault_args));
  fa->uffd = uffd;
  fa->nvm_fd = nvmfd;
  fa->dram_fd = dramfd;
  fa->nvm_to_dram = !dram;
  fa->region = p;
  fa->size = HUGEPAGE_SIZE;
  int s = pthread_create(&fault_thread, NULL, handle_fault, (void*)fa);
  if (s != 0) {
    perror("pthread_create");
    assert(0);
  }

  printf("initializing thread data\n");
  for (i = 0; i < threads; ++i) {
    ga[i] = (struct gups_args*)malloc(sizeof(struct gups_args));
    ga[i]->field = p + (i * nelems * elt_size);
    //printf("Thread [%d] starting address: %llx\n", i, ga[i]->field);
    //printf("thread %d start address: %llu\n", i, (unsigned long)td[i].field);
    ga[i]->indices = (unsigned long*)malloc(updates * sizeof(unsigned long));
    if (ga[i]->indices == NULL) {
      perror("malloc");
      exit(1);
    }
    calc_indices(ga[i]->indices, updates, nelems);
  }
  
  gettimeofday(&stoptime, NULL);
  secs = elapsed(&starttime, &stoptime);
  printf("Initialization time: %.4f seconds.\n", secs);

  printf("Timing.\n");
  pthread_t t[threads];
  pthread_t remap_thread;
  gettimeofday(&starttime, NULL);
  
  // spawn gups worker threads
  for (i = 0; i < threads; i++) {
    //printf("starting thread [%d]\n", i);
    ga[i]->tid = i; 
    ga[i]->iters = updates;
    ga[i]->size = nelems;
    ga[i]->elt_size = elt_size;
    //printf("  tid: [%d]  iters: [%llu]  size: [%llu]  elt size: [%llu]\n", ga[i]->tid, ga[i]->iters, ga[i]->size, ga[i]->elt_size);
    int r = pthread_create(&t[i], NULL, do_gups, (void*)ga[i]);
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

#ifdef EXAMINE_PGTABLES
  pthread_t pagetable_thread;
  int r = pthread_create(&pagetable_thread, NULL, examine_pagetables, NULL);
  assert(r == 0);

  r = pthread_join(pagetable_thread, NULL);
  assert(r == 0);
#endif

  for (i = 0; i < threads; i++) {
    free(ga[i]->indices);
    free(ga[i]);
  }
  free(ga);

  close(nvmfd);
  close(dramfd);
  munmap(p, size);

  return 0;
}
