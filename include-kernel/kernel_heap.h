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

#ifndef KERNEL_HEAP_H
#define KERNEL_HEAP_H

#include <stdint.h>
#include "isr.h"
#include "vmm.h"

//Kernel heap живёт в отдельном VA-range, а НЕ в identity-mapping
//Причина: identity heap на VA~2MB+ пересекается с динамическими
//kernel stack (PA=VA 16MB+), и при росте heap перезаписывает их PTE.
//VA 0x10000000 = 256MB → PDE 64, вне identity (PDE 0..31 для 128MB)
//и вне user-range (256..511). Каждая новая PDE -> propagation в user-PD

//#define KERNEL_HEAP_VA_BASE   0x80000000u    //2 GB,  PDE 512
//#define KERNEL_HEAP_VA_LIMIT  0xC0000000u    //3 GB,  PDE 767 inclusive
// kernel_heap.h

#define KERNEL_HEAP_VA_BASE   0x80000000u
#define KERNEL_HEAP_VA_LIMIT  0xB0000000u   //было 0xC0000000, теперь до 0xB0000000
//(2GB..2.75GB heap, оставляет 2.75GB..3GB свободным под MMIO)

extern uint32_t mem_ptr;
extern kmem_block_t *free_list;
extern uint32_t mem_ptr;

extern uint32_t heap_next_va;

void* kmalloc(uint32_t size);
void kfree(void *ptr);

#endif
