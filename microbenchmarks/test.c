/*
 * =====================================================================================
 *
 *       Filename:  test.c
 *
 *    Description:  
 *
 *        Version:  1.0
 *        Created:  03/17/2020 06:24:25 AM
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

#include "../timer.h"
#include "../hemem.h"

#define KB(x)     ((uint64_t)x * 1024)
#define MB(x)     (KB(x) * 1024)
#define GB(x)     (MB(x) * 1024)

#define SIZE      (GB(256))

int main(int argc, char **argv)
{
  void *p;
  uint64_t i;
  uint64_t *region;
  uint64_t nelems;
  struct timeval start, end;
  uint64_t startval;

  if (argc != 2) {
    printf("usage: %s val\n", argv[0]);
    return 0;
  }

  startval = atoi(argv[1]);

  p = mmap(NULL, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    perror("mmap");
    assert(0);
  }

  region = (uint64_t*)p;
  nelems = (SIZE / sizeof(uint64_t));
  printf("there are %lu elements\n", nelems);

  gettimeofday(&start, NULL);
  for (i = 0; i < nelems; i++) {
    region[i] = startval;
    if (region[i] != startval) {
      assert(region[i] == startval);
    }
  }
  gettimeofday(&end, NULL);
  printf("init region took %.4f seconds\n", elapsed(&start, &end));
  hemem_print_stats();

  for (i = 0; i < nelems; i++) {
    if (region[i] != startval) {
      assert(region[i] == startval);
    }
  }
  hemem_print_stats();

  gettimeofday(&start, NULL);
  for (i = 0; i < nelems; i++) {
    region[i] = region[i] + 2;
    if (region[i] != startval + 2) {
      assert(region[i] == startval + 2);
    }
  }
  gettimeofday(&end, NULL);
  printf("calc region took %.4f seconds\n", elapsed(&start, &end));
  hemem_print_stats();

  for (i = 0; i < nelems; i++) {
    if (region[i] != startval + 2) {
      assert(region[i] == startval + 2);
    }
  }
  hemem_print_stats();

  return 0;
}
