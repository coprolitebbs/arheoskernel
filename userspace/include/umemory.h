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

#ifndef UMEMORY_H
#define UMEMORY_H

#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE 4096

//Структура заголовка блока памяти в Userspace
typedef struct u_mem_block {
    size_t size;
    struct u_mem_block *next;
} u_mem_block_t;

typedef struct {
    uint32_t total_pages;      //Всего страниц ОЗУ, найденных при старте (от GRUB)
    uint32_t free_pages;       //Свободно физических страниц в PMM прямо сейчас
    uint32_t used_pages;       //Занято физических страниц процессами и ядром
    uint32_t heap_total_bytes; //Общий размер кучи ядра (kmalloc region) в байтах
    uint32_t heap_used_bytes;  //Сколько байт кучи занято системными объектами
} am_mem_info_t;

void* u_alloc_page(void);
void* u_sbrk(int32_t increment);
int u_free_page(void *addr);
int get_mem_info(am_mem_info_t *info);

void* malloc_pages(size_t size);
void* malloc(size_t size);
void free(void *ptr);
void free_pages(void *ptr, size_t size);



#endif
