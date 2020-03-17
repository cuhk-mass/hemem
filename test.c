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

#include "timer.h"
#include "hemem.h"

#define KB(x)     ((uint64_t)x * 1024)
#define MB(x)     (KB(x) * 1024)
#define GB(x)     (MB(x) * 1024)

#define SIZE      (GB(32))

int main(int argc, char **argv)
{
  void *p;
  uint64_t i;
  uint64_t *region;
  uint64_t nelems;
  struct timeval start, end;

  p = mmap(NULL, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    perror("mmap");
    assert(0);
  }

  region = (uint64_t*)p;
  nelems = (SIZE / sizeof(uint64_t));

  gettimeofday(&start, NULL);
  for (i = 0; i < nelems; i++) {
    region[i] = 1;
    if (region[i] != 1) {
      printf("set 1 loop: {%p} : region[%lu]: %lu != 1\n", &region[i], i, region[i]);
      assert(region[i] == 1);
    }
  }
  gettimeofday(&end, NULL);
  printf("init region took %.4f seconds\n", elapsed(&start, &end));
  hemem_print_stats();

  for (i = 0; i < nelems; i++) {
    if (region[i] != 1) {
      printf("check 1 loop: {%p} : region[%lu]: %lu != 1\n", &region[i], i, region[i]);
      assert(region[i] == 1);
    }
  }
  hemem_print_stats();

  gettimeofday(&start, NULL);
  for (i = 0; i < nelems; i++) {
    region[i] = region[i] + 2;
    if (region[i] != 3) {
      printf("set 3 loop: {%p} : region[%lu]: %lu != 3\n", &region[i], i, region[i]);
      assert(region[i] == 3);
    }
  }
  gettimeofday(&end, NULL);
  printf("calc region took %.4f seconds\n", elapsed(&start, &end));
  hemem_print_stats();

  for (i = 0; i < nelems; i++) {
    if (region[i] != 3) {
      printf("check 3 loop: {%p} : region[%lu]: %lu != 3\n", &region[i], i, region[i]);
      assert(region[i] == 3);
    }
  }
  hemem_print_stats();

  return 0;
}
