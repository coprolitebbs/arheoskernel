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

#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include <stddef.h>

//Флаги страниц
#define PAGE_SIZE 4096
#define PAGE_PRESENT 0x01
#define PAGE_WRITE   0x02
#define PAGE_USER    0x04
#define PAGE_SIZE_4MB 0x80
#define PAGE_PWT     0x08   //Page Write-Through (bit 3)
#define PAGE_PCD     0x10   //Page Cache Disable (bit 4)


typedef struct vm_page{
    uint32_t virt;
    uint32_t phys;
    uint32_t flags;
    struct vm_page *next;
} vm_page_t;

typedef struct kmem_block{
    uint32_t size;
    struct kmem_block *next;
} kmem_block_t;

//Таблица страниц — массив из 1024 записей
typedef uint32_t page_table_t[1024];
//Каталог страниц — массив из 1024 записей (указатели на таблицы)
typedef uint32_t page_directory_t[1024];

extern page_directory_t *kernel_directory;

/* Заполнить один kernel-PDE в user-PD. Идемпотентна. */
void vmm_sync_pde_into_user_pd(page_directory_t *user_pd, uint32_t pd_index);

/* Заполнить ВСЕ kernel-PDE в user-PD. Идемпотентна. */
void vmm_sync_kernel_pdes(page_directory_t *user_pd);

//Создать новое адресное пространство (каталог страниц)
page_directory_t* vmm_create_address_space(uint32_t *phys_out);

void vmm_destroy_address_space(page_directory_t *dir);

//Отобразить виртуальный адрес на физический
void vmm_map_page(page_directory_t *dir, uint32_t virt, uint32_t phys, uint32_t flags);

void vmm_map_pages_uncached(uint32_t phys, uint32_t size);

//Убрать отображение
void vmm_unmap_page(page_directory_t *dir, uint32_t virt);

//Получить физический адрес по виртуальному
uint32_t vmm_get_phys_addr(page_directory_t *dir, uint32_t virt);

//Переключить каталог страниц
void vmm_switch_directory(uint32_t phys);


//Инициализировать VMM (создать каталог ядра и включить paging)
void vmm_init(/*uint32_t vbe_phys, uint32_t vbe_size*/);

void vmm_map_pages(uint32_t vbe_phys, uint32_t vbe_size);

/* Явно пометить PDE как kernel-critical (LFB/MMIO): копируется в user-PD
   даже из диапазона 256..511. Вызывается из vmm_map_pages() автоматически
   для LFB; для других MMIO-регионов — вручную после vmm_map_page(). */
void vmm_mark_pde_critical(uint32_t pd_index);

//Выделить страницу в виртуальном адресном пространстве ядра
void* vmm_alloc_kernel_page(void);

//Освободить страницу в виртуальном адресном пространстве ядра
void vmm_free_kernel_page(void *addr);

page_directory_t* vmm_create_user_space(void);

void vmm_map_user_page(page_directory_t *dir,uint32_t virt,uint32_t phys);

page_directory_t* vmm_get_kernel_directory(void);

void vmm_copy_kernel_space(page_directory_t *dir);

uint32_t vmm_get_last_directory_phys(void);

#endif
