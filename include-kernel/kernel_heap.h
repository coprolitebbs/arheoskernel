#ifndef KERNEL_HEAP_H
#define KERNEL_HEAP_H

#include <stdint.h>
#include "isr.h"
#include "vmm.h"

extern uint32_t mem_ptr;
extern kmem_block_t *free_list;

void* kmalloc(uint32_t size);
void kfree(void *ptr);

#endif
