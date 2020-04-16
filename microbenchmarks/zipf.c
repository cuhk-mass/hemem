/*
 * =====================================================================================
 *
 *       Filename:  zipf.c
 *
 *    Description:  
 *
 *        Version:  1.0
 *        Created:  05/06/2019 11:24:53 AM
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  YOUR NAME (), 
 *   Organization:  
 *
 * =====================================================================================
 */
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include <assert.h>
#include <stdint.h>

#include "gups.h"

#ifdef ZIPFIAN

static const double ZETAN = 26.46902820178302;
static const double ZIPFIAN_CONSTANT = 0.99;
static unsigned long min, max, itemcount;
static unsigned long items, base, countforzeta;
static double zipfianconstant, alpha, zetan, eta, theta, zeta2theta;
static unsigned long lastVal;
static int allowitemdecrease = 0;
static const long FNV_OFFSET_BASIS_64 = 0xCBF29CE484222325L;
static const long FNV_PRIME_64 = 1099511628211L;

static unsigned long fnvhash64(unsigned long val) {
  long hashval = FNV_OFFSET_BASIS_64;

  for (int i = 0; i < 8; i++) {
    long octet = val & 0x00ff;
    val = val >> 8;

    hashval = hashval ^ octet;
    hashval = hashval * FNV_PRIME_64;
  }

  return (unsigned long)abs(hashval);
}

static double _zetastatic(unsigned long st, unsigned long n, double theta, double initialsum)
{
  double sum = initialsum;
  for (unsigned long i = st; i < n; i++) {
    sum += 1 / (pow(i + 1, theta));
  }
  return sum;
}

static double _zeta(unsigned long st, unsigned long n, double thetaVal, double initialsum)
{
  countforzeta = n;
  return _zetastatic(st, n, thetaVal, initialsum);
}

static double zetastatic(unsigned long n, double theta)
{
  return _zetastatic(0, n, theta, 0);
}

static double zeta(unsigned long n, double thetaVal)
{
  countforzeta = n;
  return zetastatic(n, thetaVal);

}

static unsigned long nextValue(unsigned long itemcount)
{
  if (itemcount != countforzeta) {
    if (itemcount > countforzeta) {
      printf("recomputing zeta due to item increase\n");
      zetan = _zeta(countforzeta, itemcount, theta, zetan);
      eta = (1 - pow(2.0 / items, 1 - theta)) / (1 - zeta2theta / zetan);
    } else if (itemcount > countforzeta) {
      printf("recomputing zeta due to item decrease (warning: slow)\n");
      zetan = zeta(itemcount, theta);
      eta = (1 - pow(2.0 / items, 1 - theta)) / (1 - zeta2theta / zetan);
    }
  }

  double u = (double)rand() / RAND_MAX;
  double uz = u * zetan;

  if (uz < 1.0) {
    return base;
  }

  if (uz < 1.0 + pow(0.5, theta)) {
    return base + 1;
  }

  unsigned long ret = base + (unsigned long)((itemcount) * pow(eta * u - eta + 1, alpha));
  lastVal = ret;
  return ret;
}

void calc_indices(unsigned long* indices, unsigned long updates, unsigned long nelems)
{
  FILE* f;
  unsigned int i;

  assert(!"Not thread-safe");
  
  f = fopen(INDEX_FILE, "w");
  if (f == NULL) {
    perror("fopen");
    assert(0);
  }
  assert(indices != NULL);

  // init zipfian distrobution variables
  min = 0;
  max = nelems - 1;
  itemcount = max - min + 1;
  items = max - min + 1;
  base = min;
  zipfianconstant = ZIPFIAN_CONSTANT;
  theta = zipfianconstant;
  zeta2theta = zeta(2, theta);

  alpha = 1.0 / (1.0 - theta);
  zetan = ZETAN;
  countforzeta = items;
  eta = (1 - pow(2.0 / items, 1 - theta)) / (1 - zeta2theta / zetan);
  nextValue(nelems);

  for (i = 0; i < updates; i++) {
    unsigned long ret = nextValue(nelems);
    ret = min + fnvhash64(ret) % itemcount;
    lastVal = ret;
    indices[i] = ret;
    //fprintf(f, "%d\n", indices[i]);
  }

  fclose(f);
}

#elif defined HOTSPOT

#define RAND_WITHIN(x)	(((double)rand_r(&seed) / RAND_MAX) * (x))

uint64_t hotset_start = 0;
double hotset_fraction = 0.1;
static double hotset_prob = 0.9;

void calc_indices(unsigned long* indices, unsigned long updates, unsigned long nelems)
{
  int i;
  uint64_t hotset_size = (uint64_t)(hotset_fraction * nelems);
  unsigned int seed = 0;

  assert(hotset_start + hotset_size <= nelems);

  printf("hotset start: %lu\thotset size: %lu\thotset probability: %f\n", hotset_start, hotset_size, hotset_prob);
  
  /* srand(0); */

  for (i = 0; i < updates; i++) {
    if (RAND_WITHIN(1) < hotset_prob) {
      indices[i] = hotset_start + (uint64_t)RAND_WITHIN(hotset_size);
    }
    else {
      indices[i] = (uint64_t)RAND_WITHIN(nelems);
    }
  }
}

#else // UNIFORM_RANDOM

void calc_indices(unsigned long* indices, unsigned long updates, unsigned long nelems)
{
  unsigned int i;
  assert(indices != NULL);
  unsigned int seed = 0;

  /* srand(0); */

  for (i = 0; i < updates; i++) {
    indices[i] = rand_r(&seed) % nelems;
  }
}

#endif
