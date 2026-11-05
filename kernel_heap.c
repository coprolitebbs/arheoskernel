/*
 * Copyright (C) 2026 ARHeOS by Dmitry H
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

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
#include "include-kernel/comdebug.h"

static spinlock_t heap_spinlock = {0};

uint32_t heap_next_va = KERNEL_HEAP_VA_BASE;

//uint32_t mem_ptr = 0x600000; // начало свободной памяти (после ядра)
kmem_block_t *free_list = NULL;

uint32_t mem_ptr = 0;


void* kmalloc(uint32_t size)
{
    if(size == 0) return NULL;

    uint32_t eflags = spin_lock_irqsave(&heap_spinlock);

    //Разовый sanity-check при первом вызове: kernel heap VA не должен пересекаться с LFB
    //(identity VA == PA). иначе kmalloc перезапишет PTE для LFB, и драйвер FB начнёт писать в кучу
    if (heap_next_va == KERNEL_HEAP_VA_BASE)
    {
        //extern uint32_t fb_phys_addr;
        if (graphics_get_fb_phys_addr() != 0)
        {
            uint32_t fb_end = fb_phys_addr + 4u * 1024u * 1024u;
            //если LFB внезапно оказался в диапазоне heap — сдвигаем heap вверх, за LFB, вместо того чтобы падать в NULL
            if (graphics_get_fb_phys_addr() < KERNEL_HEAP_VA_LIMIT && fb_end > KERNEL_HEAP_VA_BASE)
            {
                uint32_t new_base = (fb_end + 0xFFFFF) & ~0xFFFFF;
                //com_puts("[HEAP] WARN: LFB in heap range, shift heap to ");
                //com_puthex(new_base); com_puts("\n");
                heap_next_va = new_base;
            }
        }
    }

    size = (size + 3) & ~3;
    kmem_block_t *prev = NULL;
    kmem_block_t *cur = free_list;
    while(cur)
    {
        if(cur->size >= size)
        {
            if(prev) prev->next = cur->next;
            else free_list = cur->next;

            //Сплиттинг: если блок заметно больше запроса, отрезаем хвост и возвращаем его в
            //free_list, 64 байта — минимальный остаток, который имеет смысл держать
            //отдельным блоком (заголовок + выравнивание)
            const uint32_t MIN_SPLIT_TAIL = 64;
            if (cur->size >= size + sizeof(kmem_block_t) + MIN_SPLIT_TAIL)
            {
                kmem_block_t *tail = (kmem_block_t*)((uint8_t*)(cur + 1) + size);
                tail->size = cur->size - size - sizeof(kmem_block_t);
                tail->next = free_list;
                free_list = tail;
                cur->size = size;
            }

            //Зануляем ТОЛЬКО запрошенный размер, блок может быть огромным (после kfree 8MB),
            //и memset по cur->size убивает производительность (768×8MB при чтении больших файлов)
            //Хвост блока занулится при следующей выдаче из того же места — он всё равно недоступен вызывающему
            memset((void*)(cur + 1), 0, size);
            spin_unlock_irqrestore(&heap_spinlock, eflags);
            return (void*)(cur + 1);
        }
        prev = cur;
        cur = cur->next;
    }

    uint32_t total_needed = sizeof(kmem_block_t) + size;
    uint32_t page_aligned_size = (total_needed + 4095) & ~4095;
    uint32_t pages_count = page_aligned_size / PAGE_SIZE;
    uint32_t start_virt_addr = heap_next_va;

    if (start_virt_addr + page_aligned_size >= KERNEL_HEAP_VA_LIMIT)
    {
        spin_unlock_irqrestore(&heap_spinlock, eflags);
        return NULL;
    }

    for (uint32_t i = 0; i < pages_count; i++)
    {
        uint32_t current_virt = start_virt_addr + (i * PAGE_SIZE);
        uint32_t current_phys = (uint32_t)pmm_alloc_page();
        if (!current_phys)
        {
            spin_unlock_irqrestore(&heap_spinlock, eflags);
            return NULL;
        }
        //PCI-hole защита (pmm уже вырезает, но пусть будет)
        /*if (current_phys >= 0x40000000 && current_phys < 0xC0000000){
            pmm_free_page((void*)current_phys);
            spin_unlock_irqrestore(&heap_spinlock, eflags);
            return NULL;
        }*/
        vmm_map_page(kernel_directory, current_virt, current_phys,
                     PAGE_PRESENT | PAGE_WRITE);
        memset((void*)current_virt, 0, PAGE_SIZE);
    }

    kmem_block_t *block = (kmem_block_t*)start_virt_addr;
    block->size = page_aligned_size - sizeof(kmem_block_t);
    block->next = NULL;
    heap_next_va += page_aligned_size;
    spin_unlock_irqrestore(&heap_spinlock, eflags);
    return (void*)(block + 1);
}

void kfree(void *ptr)
{
    if (!ptr) return;
    uint32_t eflags = spin_lock_irqsave(&heap_spinlock);
    kmem_block_t *block = ((kmem_block_t*)ptr) - 1;
    if (((uint32_t)block & 7) != 0)
    {
        spin_unlock_irqrestore(&heap_spinlock, eflags);
        return;
    }
    if ((uint32_t)block < KERNEL_HEAP_VA_BASE ||
            (uint32_t)block >= heap_next_va)
    {
        spin_unlock_irqrestore(&heap_spinlock, eflags);
        return;
    }
    block->next = free_list;
    free_list = block;
    spin_unlock_irqrestore(&heap_spinlock, eflags);
}

