#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>
#include "include-kernel/lib.h"

#define PAGE_SIZE 4096


// ---- Структура записи E820 (для совместимости) ----
struct e820_entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t acpi;
} __attribute__((packed));

// Инициализация: принять карту памяти из E820 (массив записей и их количество)
void pmm_init(uint16_t num_entries, struct e820_entry *entries);

// Выделить одну физическую страницу (возвращает физический адрес)
void *pmm_alloc_page(void);

// Освободить физическую страницу
void pmm_free_page(void *addr);

// Выделить несколько непрерывных страниц (для DMA)
void *pmm_alloc_pages(size_t count);

// Освободить несколько страниц
void pmm_free_pages(void *addr, size_t count);

// Получить общий размер свободной памяти
size_t pmm_get_free_memory(void);

// Получить общий размер памяти
size_t pmm_get_total_memory(void);

#endif