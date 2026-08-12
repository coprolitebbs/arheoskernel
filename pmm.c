#include "include-kernel/pmm.h"
#include <stddef.h>
#include "include-kernel/lib.h"   // вместо <string.h>
#include "include-kernel/draw.h"

// ---- Глобальные переменные ----
static uint8_t *bitmap = NULL;
static size_t total_pages = 0;
static size_t free_pages = 0;
static uint32_t memory_start = 0;


void itoa2(int value, char *str, int base) {
    char *ptr = str, *ptr1 = str, tmp_char;
    int tmp_value;

    do {
        tmp_value = value;
        value /= base;
        *ptr++ = "0123456789"[tmp_value - value * base];
    } while (value);

    *ptr-- = '\0';
    while (ptr1 < ptr) {
        tmp_char = *ptr;
        *ptr-- = *ptr1;
        *ptr1++ = tmp_char;
    }
}

void pmm_init(uint16_t num_entries, struct e820_entry *entries) {
    // ---- Фиксированный размер памяти для отладки ----
    uintptr_t min_addr = 0x100000;          // 1 МБ
    uintptr_t max_addr = 128 * 1024 * 1024; // 128 МБ
    uintptr_t total_mem = max_addr - min_addr;

    total_pages = total_mem / PAGE_SIZE;
    memory_start = min_addr;

    // ---- Размещаем битовую карту после ядра (фиксированный адрес 0x200000) ----
    uintptr_t kernel_end = 0x200000; // 2 МБ (достаточно для ядра)
    bitmap = (uint8_t*)((kernel_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));

    size_t bitmap_size = (total_pages + 7) / 8;

    // ---- Инициализация битовой карты ----
    memset(bitmap, 0x00, bitmap_size);  // все биты = 0 (занято)

    // ---- Устанавливаем биты для всех страниц как свободные ----
    for (size_t i = 0; i < total_pages; i++) {
        bitmap[i / 8] |= (1 << (i % 8));
    }

    // ---- Резервируем занятые области ----
    // 1. Страницы до min_addr (0 – 1 МБ)
    size_t reserved_before = min_addr / PAGE_SIZE;
    for (size_t i = 0; i < reserved_before && i < total_pages; i++) {
        bitmap[i / 8] &= ~(1 << (i % 8));
    }

    // 2. Сама битовая карта
    size_t bitmap_pages = (bitmap_size + PAGE_SIZE - 1) / PAGE_SIZE;
    uintptr_t bitmap_start = (uintptr_t)bitmap;
    for (size_t i = 0; i < bitmap_pages; i++) {
        size_t page_index = (bitmap_start + i * PAGE_SIZE - min_addr) / PAGE_SIZE;
        if (page_index < total_pages) {
            bitmap[page_index / 8] &= ~(1 << (page_index % 8));
        }
    }

    // 3. Ядро (от 0x100000 до конца ядра, включая _end)
    //    Используем тот же адрес kernel_end, чтобы не резервировать лишнего
    for (uintptr_t addr = 0x100000; addr < kernel_end; addr += PAGE_SIZE) {
        size_t page_index = (addr - min_addr) / PAGE_SIZE;
        if (page_index < total_pages) {
            bitmap[page_index / 8] &= ~(1 << (page_index % 8));
        }
    }

    // ---- Подсчёт свободных страниц ----
    free_pages = 0;
    for (size_t i = 0; i < total_pages; i++) {
        if (bitmap[i / 8] & (1 << (i % 8))) free_pages++;
    }

}

// ---- Выделение одной страницы ----
void *pmm_alloc_page(void) {
    if (free_pages == 0) return NULL;

    for (size_t i = 0; i < total_pages; i++) {
        if (bitmap[i / 8] & (1 << (i % 8))) {
            bitmap[i / 8] &= ~(1 << (i % 8));
            free_pages--;
            return (void*)(memory_start + i * PAGE_SIZE);
        }
    }
    return NULL;
}

// ---- Освобождение страницы ----
void pmm_free_page(void *addr) {
    if (!addr) return;
    uintptr_t addr_val = (uintptr_t)addr;
    if (addr_val < memory_start || addr_val >= memory_start + total_pages * PAGE_SIZE)
        return;

    size_t page_index = (addr_val - memory_start) / PAGE_SIZE;
    /*if (page_index < total_pages) {
        bitmap[page_index / 8] |= (1 << (page_index % 8));
        free_pages++;
    }*/
    if(page_index >= total_pages)
        return;

    /*
        Страница уже свободна.
        Не увеличиваем free_pages второй раз.
    */
    if(bitmap[page_index / 8] & (1 << (page_index % 8))){
        return;
    }
    bitmap[page_index / 8] |= (1 << (page_index % 8));
    free_pages++;
}

// ---- Выделение нескольких страниц (непрерывный блок) ----
void *pmm_alloc_pages(size_t count) {
    if (count == 0 || free_pages < count) return NULL;

    for (size_t i = 0; i <= total_pages - count; i++) {
        int found = 1;
        for (size_t j = 0; j < count; j++) {
            if (!(bitmap[(i + j) / 8] & (1 << ((i + j) % 8)))) {
                found = 0;
                break;
            }
        }
        if (found) {
            for (size_t j = 0; j < count; j++) {
                bitmap[(i + j) / 8] &= ~(1 << ((i + j) % 8));
            }
            free_pages -= count;
            return (void*)(memory_start + i * PAGE_SIZE);
        }
    }
    return NULL;
}

// ---- Освобождение нескольких страниц ----
void pmm_free_pages(void *addr, size_t count) {
    if (!addr || count == 0) return;
    uintptr_t addr_val = (uintptr_t)addr;
    if (addr_val < memory_start || addr_val >= memory_start + total_pages * PAGE_SIZE)
        return;

    size_t page_index = (addr_val - memory_start) / PAGE_SIZE;
    if (page_index + count > total_pages) return;

    for (size_t i = 0; i < count; i++) {
        bitmap[(page_index + i) / 8] |= (1 << ((page_index + i) % 8));
    }
    free_pages += count;
}

// ---- Получить общий размер свободной памяти ----
size_t pmm_get_free_memory(void) {
    return free_pages * PAGE_SIZE;
}

// ---- Получить общий размер памяти ----
size_t pmm_get_total_memory(void) {
    return total_pages * PAGE_SIZE;
}


