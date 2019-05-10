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

#define PATH "/dev/dax0.0"

#define HUGEPAGE_SIZE 2097152

extern void calc_indices(unsigned long* indices, unsigned long updates, unsigned long nelems);

struct thread_data {
  unsigned long *indices;
  void *field;
};

struct thread_data* td;

struct args {
  int tid;
  struct thread_data* td;
  unsigned long iters;
  unsigned long size;
  unsigned long elt_size;
};

struct remap_args {
  void* region;
  unsigned long region_size;
  int nvm_fd;
  int base_pages;
  int nvm_to_dram;
};

void *do_remap(void *args)
{
  //printf("do_remap entered\n");
  struct remap_args *re = (struct remap_args*)args;
  void* field = re->region;
  unsigned long size = re->region_size;
  int fd = re->nvm_fd;
  int base = re->base_pages;
  int nvm_to_dram = re->nvm_to_dram;
  void *ptr = NULL;

  assert(field != NULL);
  //printf("do_remap:\tfield: 0x%x\tfd: %d\tbase: %d\tsize: %llu\tnvm_to_dram: %d\n",field, fd, base, size, nvm_to_dram);

  //TODO: figure out how to remap
  // Design:
  // wait for ~1 second
  // change protection on region to read-only
  //   how to handle faults caused by this? userfaultfd?
  // copy data to new physical address
  //   is this possible from userspace? mremap, userfaultfd, move_pages
  // keep virtual address the same, ideally
  //   just change virtual-to-physical mapping -- possible from userspace?
  // use huge pages (for now) for NVM due to devdax requirments
  //   keep huge page when moving to DRAM? When does it make sense to break up?

  sleep(1);
  
  //printf("Changing protection to read only\n");
  //int ret = mprotect(field, size, PROT_READ);
  
  //if (ret < 0) {
    //perror("mprotect");
    //assert(0);
  //}

  //printf("Protection changed\n");

  if (nvm_to_dram) {
    // move region from nvm to dram
    printf("Moving region from NVM to DRAM\n");

    ptr = mmap(NULL, HUGEPAGE_SIZE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_SHARED | MAP_POPULATE | MAP_HUGETLB, -1, 0);
    if (ptr == MAP_FAILED) {
      perror("mmap");
      assert(0);
    }

    memcpy(ptr, field, HUGEPAGE_SIZE);

    int ret = munmap(field, HUGEPAGE_SIZE);
    if (ret < 0) {
      perror("munmap");
      assert(0);
    }

    void *newptr = mremap(ptr, HUGEPAGE_SIZE, HUGEPAGE_SIZE, MREMAP_FIXED | MREMAP_MAYMOVE, field);
    if (newptr == MAP_FAILED) {
      perror("mremap");
      assert(0);
    }

  }
  else {
    // move region frm dram to nvm
    printf("Moving region from DRAM to NVM\n");

    ptr = mmap(NULL, HUGEPAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, 0);
    if (ptr == MAP_FAILED) {
      perror("mmap");
      assert(0);
    }

    memcpy(ptr, field, HUGEPAGE_SIZE);

    int ret = munmap(field, HUGEPAGE_SIZE);
    if (ret < 0) {
      perror("munmap");
      assert(0);
    }

    void *newptr = mremap(ptr, HUGEPAGE_SIZE, HUGEPAGE_SIZE, MREMAP_FIXED | MREMAP_MAYMOVE, field);
    if (newptr == MAP_FAILED) {
      perror("mremap");
      assert(0);
    }
  }

  printf("region moved\n");
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
  int fd = 0;

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

  fd = open(PATH, O_RDWR);
  if (fd < 0) {
    perror("open");
  }
  assert(fd >= 0);
  //printf("NVM fd: %d\n", fd);

  int i;
  void *p;
  if (dram) {
    // we take into account base vs. huge pages for DRAM but not NVM
    if (base) {
      p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE | MAP_ANONYMOUS, -1, 0);
    }
    else {
      p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    }
  }
  else {
    // mapping devdax mode NVM with base pages does not seem to be possible
    p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, 0);
  }

  if (p == NULL || p == MAP_FAILED) {
    perror("mmap");
  }  
  assert(p != NULL && p != MAP_FAILED);
  //printf("Field addr: 0x%x\n", p);

  nelems = (size / threads) / elt_size; // number of elements per thread

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
    re->region = p;
    re->nvm_fd = fd;
    re->base_pages = base;
    re->region_size = size;
    re->nvm_to_dram = !dram;
    //printf("field: 0x%x\tfd: %d\tbase: %d\tsize: %llu\tnvm_to_dram: %d\n", re->region, re->nvm_fd, re->base_pages, re->region_size, re->nvm_to_dram);
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

  munmap(p, size);

  return 0;
}
