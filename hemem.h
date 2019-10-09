#ifndef HEMEM_H
#define HEMEM_H

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
#include <linux/userfaultfd.h>
#include <poll.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <errno.h>

#define NVMSIZE		(128L * (1024L * 1024L * 1024L))
#define DRAMSIZE	(8L * (1024L * 1024L * 1024L))

#define DRAMPATH "/dev/dax0.0"
#define NVMPATH "/dev/dax1.0"

//#define PAGE_SIZE (1024 * 1024 * 1024)
//#define PAGE_SIZE (2 * (1024 * 1024))
#define PAGE_SIZE (4 * 1024)

pthread_t fault_thread;

int dramfd = -1;
int nvmfd = -1;
long uffd = -1;
int init = 0;
unsigned long mem_allocated = 0;
int alloc_nvm = 0;
int wp_faults_handled = 0;
int missing_faults_handled = 0;

void hemem_init();
void* hemem_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
void *handle_fault(void* arg);

#ifdef EXAMINE_PGTABLES

struct pagemapEntry {
  unsigned long long pfn : 54;
  unsigned int soft_dirty : 1;
  unsigned int exclusive : 1;
  unsigned int file_page : 1;
  unsigned int swapped : 1;
  unsigned int present : 1;
};

void *examine_pagetables();

#endif /*EXAMINE_PGTABLES*/

#endif /* HEMEM_H */
