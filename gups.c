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
#include <errno.h>

#include "timer.h"
#include "hemem.h"


#include "gups.h"

#define MAX_THREADS	64

#ifdef HOTSPOT
extern uint64_t hotset_start;
extern double hotset_fraction;
#endif

uint64_t hot_start = 0;
uint64_t hotsize;

struct gups_args {
  int tid;                      // thread id
  uint64_t *indices;       // array of indices to access
  void* field;                  // pointer to start of thread's region
  uint64_t iters;          // iterations to perform
  uint64_t size;           // size of region
  uint64_t elt_size;       // size of elements
};

static unsigned long updates, nelems;


static uint64_t lfsr_fast(uint64_t lfsr)
{
  lfsr ^= lfsr >> 7;
  lfsr ^= lfsr << 9;
  lfsr ^= lfsr >> 13;
  return lfsr;
}


static void *do_gups(void *arguments)
{
  //printf("do_gups entered\n");
  struct gups_args *args = (struct gups_args*)arguments;
  char *field = (char*)(args->field);
  uint64_t i, j;
  uint64_t index1, index2;
  uint64_t elt_size = args->elt_size;
  char data[elt_size];
  uint64_t lfsr;
  uint64_t hot_num;

  srand(0);
  lfsr = rand();

  index1 = 0;
  index2 = 0;

  for (i = 0; i < args->iters; i++) {
    hot_num = lfsr % 4 + 8;
    for (j = 0; j < hot_num; j++) {
      lfsr = lfsr_fast(lfsr);
      index1 = hot_start + (lfsr % hotsize);
      memcpy(data, &field[index1 * elt_size], elt_size);
      memset(data, data[0] + i, elt_size);
      memcpy(&field[index1 * elt_size], data, elt_size);
    }
    i += hot_num;

    lfsr = lfsr_fast(lfsr);
    index2 = lfsr % (args->size);
    memcpy(data, &field[index2 * elt_size], elt_size);
    memset(data, data[0] + i, elt_size);
    memcpy(&field[index2 * elt_size], data, elt_size);
  }
/*
  //printf("Thread [%d] starting: field: [%llx]\n", args->tid, field);
  for (i = 0; i < args->iters; i++) {
    index = args->indices[i];
    memcpy(data, &field[index * elt_size], elt_size);
    memset(data, data[0] + i, elt_size);
    memcpy(&field[index * elt_size], data, elt_size);
  }
*/
  return 0;
}
/*
static void *calc_indices_thread(void *arg)
{
  struct gups_args *gai = (struct gups_args*)arg;
  calc_indices(gai->indices, updates, nelems);
  return NULL;
}
*/
int main(int argc, char **argv)
{
  int threads;
  unsigned long expt;
  unsigned long size, elt_size;
  struct timeval starttime, stoptime;
  double secs, gups;
  int i;
  void *p;
  struct gups_args** ga;
  pthread_t t[MAX_THREADS];

  if (argc != 5) {
    printf("Usage: %s [threads] [updates per thread] [exponent] [data size (bytes)] [noremap/remap]\n", argv[0]);
    printf("  threads\t\t\tnumber of threads to launch\n");
    printf("  updates per thread\t\tnumber of updates per thread\n");
    printf("  exponent\t\t\tlog size of region\n");
    printf("  data size\t\t\tsize of data in array (in bytes)\n");
    return 0;
  }

  gettimeofday(&starttime, NULL);
  
  threads = atoi(argv[1]);
  assert(threads <= MAX_THREADS);
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

  printf("%lu updates per thread (%d threads)\n", updates, threads);
  printf("field of 2^%lu (%lu) bytes\n", expt, size);
  printf("%ld byte element size (%ld elements total)\n", elt_size, size / elt_size);

  p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);

  gettimeofday(&stoptime, NULL);
  printf("Init took %.4f seconds\n", elapsed(&starttime, &stoptime));
  printf("Region address: %p\t size: %ld\n", p, size);
  //printf("Field addr: 0x%x\n", p);

  nelems = (size / threads) / elt_size; // number of elements per thread
  printf("Elements per thread: %lu\n", nelems);

  printf("initializing thread data\n");
  for (i = 0; i < threads; ++i) {
    ga[i] = (struct gups_args*)malloc(sizeof(struct gups_args));
    ga[i]->field = p + (i * nelems * elt_size);
    //printf("Thread [%d] starting address: %llx\n", i, ga[i]->field);
    //printf("thread %d start address: %llu\n", i, (unsigned long)td[i].field);
    /*
    ga[i]->indices = (unsigned long*)malloc(updates * sizeof(unsigned long));
    if (ga[i]->indices == NULL) {
      perror("malloc");
      exit(1);
    }
    int r = pthread_create(&t[i], NULL, calc_indices_thread, (void*)ga[i]);
    assert(r == 0); 
    */
  }
  
  // wait for worker threads
  //for (i = 0; i < threads; i++) {
    //int r = pthread_join(t[i], NULL);
    //assert(r == 0);
  //}

  gettimeofday(&stoptime, NULL);
  secs = elapsed(&starttime, &stoptime);
  printf("Initialization time: %.4f seconds.\n", secs);

  hot_start = 0;
  hotsize = nelems / 10;
  printf("hot_start: %lu\thot_size: %lu\n", hot_start, hotsize);

  printf("Timing.\n");
  gettimeofday(&starttime, NULL);
  hemem_clear_stats();
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

  // wait for worker threads
  for (i = 0; i < threads; i++) {
    int r = pthread_join(t[i], NULL);
    assert(r == 0);
  }
  gettimeofday(&stoptime, NULL);
  hemem_print_stats();
  hemem_clear_stats();

  secs = elapsed(&starttime, &stoptime);
  printf("Elapsed time: %.4f seconds.\n", secs);
  gups = threads * ((double)updates) / (secs * 1.0e9);
  printf("GUPS = %.10f\n", gups);

#ifdef HOTSPOT
  hot_start = nelems - (uint64_t)(hotsize) - 1;
  printf("hot_start: %lu\thot_size: %lu\n", hot_start, hotsize);

  printf("Timing.\n");
  gettimeofday(&starttime, NULL);
  
  // spawn gups worker threads
  for (i = 0; i < threads; i++) {
    ga[i]->tid = i; 
    ga[i]->iters = updates;
    ga[i]->size = nelems;
    ga[i]->elt_size = elt_size;
    int r = pthread_create(&t[i], NULL, do_gups, (void*)ga[i]);
    assert(r == 0);
  }

  // wait for worker threads
  for (i = 0; i < threads; i++) {
    int r = pthread_join(t[i], NULL);
    assert(r == 0);
  }

  gettimeofday(&stoptime, NULL);

  secs = elapsed(&starttime, &stoptime);
  printf("Elapsed time: %.4f seconds.\n", secs);
  gups = threads * ((double)updates) / (secs * 1.0e9);
  printf("GUPS = %.10f\n", gups);
#endif
  
  for (i = 0; i < threads; i++) {
    free(ga[i]->indices);
    free(ga[i]);
  }
  free(ga);
  
  //munmap(p, size);

  hemem_print_stats();

  return 0;
}
