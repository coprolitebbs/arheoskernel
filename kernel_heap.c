#include "include-kernel/kernel_heap.h"
#include <stdint.h>
#include <stddef.h>
#include "include-kernel/draw.h"
#include "include-kernel/debug.h"
#include "include-kernel/lib.h"
#include "include-kernel/gdt.h"
#include "include-kernel/vmm.h"
#include "include-kernel/pmm.h"
#include "include-kernel/spinlock.h"
#include "include-kernel/kconsole.h"


static spinlock_t heap_spinlock = {0};

uint32_t mem_ptr = 0x600000; // начало свободной памяти (после ядра)
kmem_block_t *free_list = NULL;


void* kmalloc(uint32_t size){
    if(size == 0) return NULL;

    uint32_t eflags = spin_lock_irqsave(&heap_spinlock);

    // Выравниваем запрашиваемый размер блока по границе 4 байт
    size = (size + 3) & ~3;
    // Ищем подходящий освобождённый блок в списке повторного использования
    kmem_block_t *prev = NULL;
    kmem_block_t *cur = free_list;

    while(cur){
        if(cur->size >= size){
            if(prev) prev->next = cur->next;
            else free_list = cur->next;
            spin_unlock_irqrestore(&heap_spinlock, eflags);
            return (void*)(cur + 1);
        }
        prev = cur;
        cur = cur->next;
    }

    //Вычисляем суммарный объем ОЗУ для новой аллокации (заголовок + тело)
    uint32_t total_needed = sizeof(kmem_block_t) + size;
    //Округляем весь системный запрос до целого числа физических страниц PAGE_SIZE (4096 байт)
    uint32_t page_aligned_size = (total_needed + 4095) & ~4095;
    uint32_t pages_count = page_aligned_size / PAGE_SIZE;
    //Фиксируем стартовый виртуальный и физический адрес нового региона кучи ядра
    uint32_t start_virt_addr = mem_ptr;
    //В каноничном Identity Mapping ядра виртуальный адрес равен физическому.
    //Запрашиваем у PMM необходимое количество страниц подряд
    for (uint32_t i = 0; i < pages_count; i++) {
        uint32_t current_virt = start_virt_addr + (i * PAGE_SIZE);
        //Запрашиваем физическую страницу из безопасного пула ядра
        uint32_t current_phys = (uint32_t)pmm_alloc_page();
        //kprintf("page: %d current_phys: %x\n",i,current_phys);

        if (!current_phys) {
            //Если в куче Ring 0 кончилась память — аварийно выходим
            spin_unlock_irqrestore(&heap_spinlock, eflags);
            return NULL;
        }
        //Чисто зануляем страницу ОЗУ перед маппингом
        memset((void*)current_phys, 0, PAGE_SIZE);

        //Прописываем маппинг страницы напрямую в глобальный каталог страниц ядра
        extern page_directory_t *kernel_directory;
        vmm_map_page(kernel_directory, current_virt, current_phys, PAGE_PRESENT | PAGE_WRITE);
    }
    //Размещаем дескриптор блока памяти в начале выделенного монолитного региона
    kmem_block_t *block = (kmem_block_t*)start_virt_addr;
    block->size = page_aligned_size - sizeof(kmem_block_t);
    block->next = NULL;
    //Сдвигаем глобальный указатель кучи ядра строго за границу выделенных страниц
    mem_ptr += page_aligned_size;
    spin_unlock_irqrestore(&heap_spinlock, eflags);
    //Возвращаем указатель на полезные данные (строго за заголовком kmem_block_t)
    return (void*)(block + 1);
}

void kfree(void *ptr){
    if(!ptr) return;
    uint32_t eflags = spin_lock_irqsave(&heap_spinlock);
    //Возвращаем блок в локальный список кучи ядра для повторного использования мелких чанков
    kmem_block_t *block = ((kmem_block_t*)ptr) - 1;
    block->next = free_list;
    free_list = block;
    spin_unlock_irqrestore(&heap_spinlock, eflags);
}

