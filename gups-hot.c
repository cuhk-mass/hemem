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
#include <stdint.h>
#include <libpmem.h>

#include "timer.h"
#include "hemem.h"

extern void calc_indices(unsigned long* indices, unsigned long updates, unsigned long nelems);

struct gups_args {
  int tid;                      // thread id
  unsigned long *indices;       // array of indices to access
  void* field;                  // pointer to start of thread's region
  unsigned long iters;          // iterations to perform
  unsigned long size;           // size of region
  unsigned long elt_size;       // size of elements
  unsigned long incr;
  unsigned long page_lines;
};


/* unsigned lfsr3(void)
{
    uint16_t start_state = 0xACE1u;  
    uint16_t lfsr = start_state;
    unsigned period = 0;

    do
    {
        lfsr ^= lfsr >> 7;
        lfsr ^= lfsr << 9;
        lfsr ^= lfsr >> 13;
        ++period;
    }
    while (lfsr != start_state);

    return period;
}*/

unsigned long long lfsr_fast(unsigned long long lfsr){
        lfsr ^= lfsr >> 7;
        lfsr ^= lfsr << 9;
        lfsr ^= lfsr >> 13;
	return lfsr;
}
void
*do_gups(void *arguments)
{
  //printf("do_gups entered\n");
  struct gups_args *args = (struct gups_args*)arguments;
  char *field = (char*)(args->field);
  unsigned long i, j;
  unsigned long long index1, index2 = 0;
  unsigned long elt_size = args->elt_size;
  char data[elt_size];
  unsigned long jump = 1037 * 4096;
  unsigned long offset;
  unsigned long hotsize;
  unsigned long long bytes = args->size * elt_size;

  srand(time(0));
  unsigned long long lfsr = rand();
  unsigned long long lfsr2 = 0;
  unsigned long hot_num = 0;
  unsigned long page_lines = args->page_lines;
  unsigned long incr = args->incr;
  unsigned long long base;

  if (elt_size == 2097152){
  printf("Thread [%d] starting special page: field: [%llx], size %llu\n", args->tid, field, bytes);
  printf("incr: %lu \t page_lines: %lu\n", incr, page_lines);
	elt_size=64;
	index1 = lfsr % (args->size);
	for (i = 0; i < args->iters; i+=incr) {
		base = index1 * 2097152;
		for(j = 0; j<incr; j++){
			//do {
			//    lfsr2 = lfsr_fast(lfsr);
			//} while (lfsr2 - lfsr < 4);
			
			lfsr = lfsr_fast(lfsr);
			memcpy(data, &field[base + (lfsr % page_lines) * elt_size], elt_size);
    			memset(data, data[0] + i, elt_size);
    			memcpy(&field[base + (lfsr % page_lines) * elt_size], data, elt_size);
		}
		lfsr = lfsr_fast(lfsr);
    		index1 = lfsr % (args->size);
  	}
  } else {
  	printf("Thread [%d] starting: field: [%llx]\n", args->tid, field);
#ifdef HOTSET
	printf("Hot set\n");
	index1 = 0;
	hotsize = (args->size)/10;
#endif
	index2 = 0;
	for (i = 0; i < args->iters; i++) {

#ifdef HOTSET
	 	hot_num = lfsr % 4 + 8;
		for(j = 0; j<hot_num; j++){
			lfsr = lfsr_fast(lfsr);
    			index1 = lfsr % hotsize;
			memcpy(data, &field[index1 * elt_size], elt_size);
    			memset(data, data[0] + i, elt_size);
    			memcpy(&field[index1 * elt_size], data, elt_size);
		}
		i+=hot_num;
#endif
		//printf("1: %p\n", data);
		//printf("2: %p\n", field);
		//printf("3: %llu\n", index2);
		//printf("4: %lu\n", elt_size);
		lfsr = lfsr_fast(lfsr);
    		index2 = lfsr % (args->size);
		memcpy(data, &field[index2 * elt_size], elt_size);
    		memset(data, data[0] + i, elt_size);
    		memcpy(&field[index2 * elt_size], data, elt_size);
  	}
  }

  return 0;
}


int
main(int argc, char **argv)
{
  int threads;
  unsigned long updates, expt;
  unsigned long size, elt_size, nelems, incr, page_lines;
  struct timeval starttime, stoptime;
  double secs, gups;
  int i;
  void *p;
  struct gups_args** ga;

  if (argc < 5) {
    printf("Usage: %s [threads] [updates per thread] [exponent] [data size (bytes)] \n", argv[0]);
    printf("  threads\t\t\tnumber of threads to launch\n");
    printf("  updates per thread\t\tnumber of updates per thread\n");
    printf("  exponent\t\t\tlog size of region\n");
    printf("  data size\t\t\tsize of data in array (in bytes)\n");
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
  
  if(argc > 5){
    incr = atoi(argv[5]);
    page_lines = atoi(argv[6]);
  }
  //hemem_init();

  printf("%lu updates per thread (%d threads)\n", updates, threads);
  printf("field of 2^%lu (%lu) bytes\n", expt, size);
  printf("%ld byte element size (%ld elements total)\n", elt_size, size / elt_size);

  //p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  //p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
  p = pmem_map_file("/mnt/pmem12/test.txt", size, PMEM_FILE_CREATE, 0644, 0, 0);


  gettimeofday(&stoptime, NULL);
  printf("Init took %.4f seconds\n", elapsed(&starttime, &stoptime));
  printf("Region address: %016p\t size: %ld\n", p, size);
  //printf("Field addr: 0x%x\n", p);

  nelems = (size / threads) / elt_size; // number of elements per thread
  printf("Elements per thread: %lu\n", nelems);

  printf("initializing thread data\n");
  for (i = 0; i < threads; ++i) {
    ga[i] = (struct gups_args*)malloc(sizeof(struct gups_args));
    ga[i]->field = p + (i * nelems * elt_size);
    //printf("Thread [%d] starting address: %llx\n", i, ga[i]->field);
    //printf("thread %d start address: %llu\n", i, (unsigned long)td[i].field);
    //ga[i]->indices = (unsigned long*)malloc(updates * sizeof(unsigned long));
    //if (ga[i]->indices == NULL) {
    //  perror("malloc");
    //  exit(1);
    //}
    //calc_indices(ga[i]->indices, updates, nelems);
  }
  
  gettimeofday(&stoptime, NULL);
  secs = elapsed(&starttime, &stoptime);
  printf("Initialization time: %.4f seconds.\n", secs);

  printf("Timing.\n");
  pthread_t t[threads];
  gettimeofday(&starttime, NULL);
  
  // spawn gups worker threads
  for (i = 0; i < threads; i++) {
    //printf("starting thread [%d]\n", i);
    ga[i]->tid = i; 
    ga[i]->iters = updates;
    ga[i]->size = nelems;
    ga[i]->elt_size = elt_size;
    ga[i]->incr = incr;
    ga[i]->page_lines = page_lines;
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

  secs = elapsed(&starttime, &stoptime);
  printf("Elapsed time: %.4f seconds.\n", secs);
  gups = threads * ((double)updates) / (secs * 1.0e9);
  printf("GUPS = %.10f\n", gups);
  printf("missing faults handled: %d\n", missing_faults_handled);
  printf("memory allocated through faults: %lld\n", (unsigned long long)missing_faults_handled * PAGE_SIZE);

#ifdef EXAMINE_PGTABLES
  pthread_t pagetable_thread;
  int r = pthread_create(&pagetable_thread, NULL, examine_pagetables, NULL);
  assert(r == 0);

  r = pthread_join(pagetable_thread, NULL);
  assert(r == 0);
#endif

  //walk_pagetable();

  for (i = 0; i < threads; i++) {
    //free(ga[i]->indices);
    free(ga[i]);
  }
  free(ga);

  munmap(p, size);

  return 0;
}
