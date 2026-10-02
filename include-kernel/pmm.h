#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>
#include "lib.h"

#define PAGE_SIZE 4096


//Структура записи E820 (для совместимости)
struct e820_entry{
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t acpi;
} __attribute__((packed));

typedef struct {
    uint32_t total_pages;      // Всего страниц ОЗУ, найденных при старте (от GRUB)
    uint32_t free_pages;       // Свободно физических страниц в PMM прямо сейчас
    uint32_t used_pages;       // Занято физических страниц процессами и ядром
    uint32_t heap_total_bytes; // Общий размер кучи ядра (kmalloc region) в байтах
    uint32_t heap_used_bytes;  // Сколько байт кучи занято системными объектами
} am_mem_info_t;

extern uint32_t uma_video_base;
extern uint32_t fb_virt_addr;
extern uint32_t fb_phys_addr;
extern int has_lfb;

extern size_t total_pages;
extern size_t free_pages;

//Инициализация - принять карту памяти из E820 (массив записей и их количество)
void pmm_init(uint16_t num_entries, struct e820_entry *entries);

//Выделить одну физическую страницу (возвращает физический адрес)
void *pmm_alloc_page(void);

//Освободить физическую страницу
void pmm_free_page(void *addr);

//Выделить несколько непрерывных страниц (для DMA)
void *pmm_alloc_pages(size_t count);

//Освободить несколько страниц
void pmm_free_pages(void *addr, size_t count);

//Получить общий размер свободной памяти
size_t pmm_get_free_memory(void);

//Получить общий размер памяти
size_t pmm_get_total_memory(void);

void graphics_init_uma_address(uint32_t total_mem, int lfb_available, uint32_t lfb_addr_from_boot);
uint32_t graphics_get_fb_phys_addr();
uint32_t graphics_get_fb_virt_addr(void);
int graphics_has_lfb(void);

#endif
