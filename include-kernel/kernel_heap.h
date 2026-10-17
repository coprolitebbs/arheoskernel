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

extern uint32_t mem_ptr;
extern kmem_block_t *free_list;

void* kmalloc(uint32_t size);
void kfree(void *ptr);

#endif
