#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

#include "shared.h"

static uint64_t fastmem = 0, slowmem = 0;	// Clock hands

uint64_t getmem(uint64_t addr, struct pte *pte)
{
  uint64_t ret;

  if(fastmem < FASTMEM_SIZE) {
    ret = fastmem;
    fastmem += BASE_PAGE_SIZE;
  } else {
    assert(slowmem < SLOWMEM_SIZE);
    ret = slowmem | SLOWMEM_BIT;
    slowmem += BASE_PAGE_SIZE;
  }

  return ret;
}

void getmem_init(void)
{
  // Nothing to do
}

int listnum(uint64_t framenum)
{
  return -1;
}
