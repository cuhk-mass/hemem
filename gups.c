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
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <sys/time.h>
#include <math.h>

#define ln2 0.69314718055994530942

unsigned long *indices;

#define GET_NEXT_INDEX(i,size) indices[i]

/* GUPS for 64-bit wide data, serial */
void
gups64(unsigned long *field, unsigned long iters, unsigned long size)
{
  unsigned long data;
  unsigned long i;
  unsigned long long index;

  for (i = 0; i < iters; i++) {
    index = GET_NEXT_INDEX(i, size);
    data = field[index];
    data = data + ((unsigned long)iters);
    field[index] = data;
  }
}

/* Returns the number of seconds encoded in T, a "struct timeval". */
#define tv_to_double(t) (t.tv_sec + (t.tv_usec / 1000000.0))

/* Useful for doing arithmetic on struct timevals. */
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
calc_indices(unsigned long updates, unsigned long nelems)
{
  unsigned int i;

  indices = (unsigned long *)malloc(updates * sizeof(unsigned long));
  if (!indices) {
    printf ("Error: couldn't malloc space for array of indices\n");
    assert (indices != NULL);
  }
  for (i = 0; i < updates; i++) {
    indices[i] = random() % nelems;
  }
}

int
main(int argc, char **argv)
{
  unsigned long updates;
  float expt;
  unsigned long size, elt_size, nelems;
  void *field;
  unsigned long *field64;
  struct timeval starttime, stoptime;
  double secs, gups;

  if (argc != 3) {
    printf("Usage: %s [updates] [exponent]\n", argv[0]);
    return 0;
  }
  
  updates = atol(argv[1]);
  updates -= updates % 256;
  expt = atof(argv[2]);
  assert(expt > 8.0);
  assert(updates > 0 && (updates % 256 == 0));
  size = (unsigned long) exp(expt * ln2);
  size -= (size % 256);
  assert(size > 0 && (size % 256 == 0));

  printf("%lu updates, ", updates);
  printf("field of 2^%.2f (%lu) bytes.\n", expt, size);

  field = malloc(size);
  if (field == NULL) {
    printf("Error: Failed to malloc %lu bytes.\n", size);
    assert(field != NULL);
  }

  field64 = (unsigned long*)field;
  assert(sizeof(unsigned long) == 8);
  elt_size = sizeof(unsigned long);

  printf("Element size is %lu bytes.\n", elt_size);
  nelems = size / elt_size;
  printf("Field is %lu data elements starting at 0x%08lx.\n", nelems,
	  (unsigned long) field);

  printf("Calculating indices.\n");
  calc_indices (updates, nelems);

  printf("Timing.\n");
  gettimeofday(&starttime, NULL);
  gups64(field64, updates, nelems);
  gettimeofday(&stoptime, NULL);

  secs = elapsed (&starttime, &stoptime);
  printf("Elapsed time: %.4f seconds.\n", secs);
  gups = ((double)updates) / (secs * 1.0e9);
  printf("GUPS = %.10f\n", gups);

  free(field);
  free(indices);

  return 0;
}
