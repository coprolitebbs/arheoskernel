#include "include-kernel/kernel_heap.h"
#include <stdint.h>
#include <stddef.h>
//#include "include-kernel/task.h"
#include "include-kernel/draw.h"
#include "include-kernel/debug.h"
#include "include-kernel/lib.h"
#include "include-kernel/gdt.h"
#include "include-kernel/vmm.h"
#include "include-kernel/pmm.h"

uint32_t mem_ptr = 0x500000; // начало свободной памяти (после ядра)
kmem_block_t *free_list = NULL;


void* kmalloc(uint32_t size){
    if(size == 0) return NULL;
    // Выравнивание самого блока по 4 байта
    size = (size + 3) & ~3;
    //    Ищем освобождённый блок
    kmem_block_t *prev = NULL;
    kmem_block_t *cur = free_list;

    while(cur){
        if(cur->size >= size){
            if(prev) prev->next = cur->next;
            else free_list = cur->next;
            return (void*)(cur + 1);
        }
        prev = cur;
        cur = cur->next;
    }
    // Новый блок
    kmem_block_t *block = (kmem_block_t*)mem_ptr;
    block->size = size;
    block->next = NULL;
    mem_ptr += sizeof(kmem_block_t) + size;
    // Выравниваем следующий блок
    mem_ptr = (mem_ptr + 3) & ~3;
    return (void*)(block + 1);
}


void kfree(void *ptr){
    if(!ptr) return;
    kmem_block_t *block = ((kmem_block_t*)ptr) - 1;
    block->next = free_list;
    free_list = block;
}
