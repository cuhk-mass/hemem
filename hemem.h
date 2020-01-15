#ifndef HEMEM_H

#define HEMEM_H

#include <pthread.h>
#include <stdint.h>
#include <inttypes.h>

#include "paging.h"

#define NVMSIZE		(128L * (1024L * 1024L * 1024L))
#define DRAMSIZE	(8L * (1024L * 1024L * 1024L))

#define DRAMPATH "/dev/dax0.0"
#define NVMPATH "/dev/dax1.0"

//#define PAGE_SIZE (1024 * 1024 * 1024)
//#define PAGE_SIZE (2 * (1024 * 1024))
#define PAGE_SIZE (4 * 1024)

extern pthread_t fault_thread;

extern int dramfd;
extern int nvmfd;
extern int devmemfd;
extern uint64_t base;
extern long uffd;
extern int init;
extern uint64_t mem_allocated;
extern int alloc_nvm;
extern int wp_faults_handled;
extern int missing_faults_handled;


void hemem_init();
void* hemem_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int hemem_munmap(void* addr, size_t length);
void *handle_fault(void* arg);

uint64_t hemem_va_to_pa(uint64_t va);
void hemem_clear_accessed_bit(uint64_t va);
int hemem_get_accessed_bit(uint64_t va);

#endif /* HEMEM_H */
