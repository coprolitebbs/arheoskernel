#include "include-kernel/pmm.h"
#include <stddef.h>
#include "include-kernel/lib.h"   // вместо <string.h>
#include "include-kernel/comdebug.h"
#include "include-kernel/draw.h"

// ---- Глобальные переменные ----
static uint8_t *bitmap = NULL;
size_t total_pages = 0;
size_t free_pages = 0;
static uint32_t memory_start = 0;

uint32_t fb_virt_addr = 0;
uint32_t fb_phys_addr = 0;
int has_lfb = 0;


void itoa2(int value, char *str, int base){
    char *ptr = str, *ptr1 = str, tmp_char;
    int tmp_value;

    do {
        tmp_value = value;
        value /= base;
        *ptr++ = "0123456789"[tmp_value - value * base];
    } while (value);

    *ptr-- = '\0';
    while (ptr1 < ptr){
        tmp_char = *ptr;
        *ptr-- = *ptr1;
        *ptr1++ = tmp_char;
    }
}

void pmm_init(uint16_t num_entries, struct e820_entry *entries){
    //Фиксированный размер физической памяти
    uintptr_t min_addr = 0x100000;          //1 МБ
    uintptr_t max_addr = 128 * 1024 * 1024; //128 МБ
    uintptr_t total_mem = max_addr - min_addr;
    total_pages = total_mem / PAGE_SIZE;
    memory_start = min_addr;
    //Сдвигаем границу защиты ядра до 8 Мегабайт (0x800000) для Userspace кучи
    uintptr_t kernel_end = 0x800000;
    //Размещаем битовую карту на фиксированном адресе 2 Мегабайта (0x200000).
    //лежит ВЫШЕ кода ядра, но НИЖЕ системной кучи kmalloc (6 МБ)
    bitmap = (uint8_t*)0x200000;
    size_t bitmap_size = (total_pages + 7) / 8;
    //Инициализация битовой карты: изначально помечаем все биты как занятые (0x00)
    memset(bitmap, 0x00, bitmap_size);
    //Устанавливаем биты для страниц как свободные
    for (size_t i = 0; i < total_pages; i++){
        bitmap[i / 8] |= (1 << (i % 8));
    }
    //Резервируем занятые области страницы до min_addr (0 – 1 МБ)
    size_t reserved_before = min_addr / PAGE_SIZE;
    for (size_t i = 0; i < reserved_before && i < total_pages; i++){
        bitmap[i / 8] &= ~(1 << (i % 8));
    }
    //Блокируем в карте памяти страницы, занятые самой битовой картой (на 2 МБ)
    size_t bitmap_pages = (bitmap_size + PAGE_SIZE - 1) / PAGE_SIZE;
    uintptr_t bitmap_start = (uintptr_t)bitmap;
    for (size_t i = 0; i < bitmap_pages; i++){
        size_t page_index = (bitmap_start + i * PAGE_SIZE - min_addr) / PAGE_SIZE;
        if (page_index < total_pages){
            bitmap[page_index / 8] &= ~(1 << (page_index % 8));
        }
    }
    //Блокируем в битовой карте ВСЕ страницы от 1 МБ до 8 МБ
    //Это гарантирует стопроцентную защиту кода ядра, стеков Ring 0 и системной кучи
    for (uintptr_t addr = 0x100000; addr < kernel_end; addr += PAGE_SIZE){
        size_t page_index = (addr - min_addr) / PAGE_SIZE;
        if (page_index < total_pages) {
            bitmap[page_index / 8] &= ~(1 << (page_index % 8));
        }
    }
    //Подсчёт свободных страниц
    free_pages = 0;
    for (size_t i = 0; i < total_pages; i++){
        if (bitmap[i / 8] & (1 << (i % 8))) free_pages++;
    }
}



//Выделение одной страницы
void *pmm_alloc_page(void){
    if (free_pages == 0) return NULL;

    size_t start_search_index = (16 * 1024 * 1024 - memory_start) / PAGE_SIZE;
    if (start_search_index >= total_pages) start_search_index = 0;
    //Ищем свободную страницу в безопасной высокой зоне памяти (16 МБ - 128 МБ)
    for (size_t i = start_search_index; i < total_pages; i++){
        if (bitmap[i / 8] & (1 << (i % 8))) {
            bitmap[i / 8] &= ~(1 << (i % 8));
            free_pages--;
            return (void*)(memory_start + i * PAGE_SIZE);
        }
    }
    for (size_t i = 0; i < start_search_index; i++){
        if (bitmap[i / 8] & (1 << (i % 8))) {
            bitmap[i / 8] &= ~(1 << (i % 8));
            free_pages--;
            return (void*)(memory_start + i * PAGE_SIZE);
        }
    }
    return NULL;
}

//Освобождение страницы
void pmm_free_page(void *addr) {
    if (!addr) return;
    uintptr_t addr_val = (uintptr_t)addr;
    if (addr_val < memory_start || addr_val >= memory_start + total_pages * PAGE_SIZE) return;
    size_t page_index = (addr_val - memory_start) / PAGE_SIZE;
    if(page_index >= total_pages) return;
    //Если страница уже свободна, выходим, защищая free_pages от ложного инкремента
    if(bitmap[page_index / 8] & (1 << (page_index % 8))) return;
    bitmap[page_index / 8] |= (1 << (page_index % 8));
    free_pages++;
}

//Выделение нескольких страниц (непрерывный блок)
void *pmm_alloc_pages(size_t count){
    if (count == 0 || free_pages < count) return NULL;

    for (size_t i = 0; i <= total_pages - count; i++){
        int found = 1;
        for (size_t j = 0; j < count; j++){
            if (!(bitmap[(i + j) / 8] & (1 << ((i + j) % 8)))){
                found = 0;
                break;
            }
        }
        if (found){
            for (size_t j = 0; j < count; j++){
                bitmap[(i + j) / 8] &= ~(1 << ((i + j) % 8));
            }
            free_pages -= count;
            return (void*)(memory_start + i * PAGE_SIZE);
        }
    }
    return NULL;
}

//Освобождение нескольких страниц
void pmm_free_pages(void *addr, size_t count){
    if (!addr || count == 0) return;
    uintptr_t addr_val = (uintptr_t)addr;
    if (addr_val < memory_start || addr_val >= memory_start + total_pages * PAGE_SIZE) return;
    size_t page_index = (addr_val - memory_start) / PAGE_SIZE;
    if (page_index + count > total_pages) return;

    for(size_t i = 0; i < count; i++){
        bitmap[(page_index + i) / 8] |= (1 << ((page_index + i) % 8));
    }
    free_pages += count;
}

//Получить общий размер свободной памяти
size_t pmm_get_free_memory(void){
    return free_pages * PAGE_SIZE;
}

//Получить общий размер памяти
size_t pmm_get_total_memory(void){
    return total_pages * PAGE_SIZE;
}


void graphics_init_uma_address(uint32_t total_mem, int lfb_available, uint32_t lfb_addr_from_boot){
    com_init(); //Инициализируем COM сразу
    //com_puts("[GRAPHICS] Init UMA address...\n");

    if (lfb_available && lfb_addr_from_boot != 0){
        //com_puts("[GRAPHICS] LFB available from bootloader.\n");
        fb_phys_addr = lfb_addr_from_boot;
        fb_virt_addr = lfb_addr_from_boot; // Пока считаем, что виртуальный = физический (identity map)
        has_lfb = 1;
        return;
    }

    //Если LFB нет, пытаемся найти UMA
    //com_puts("[GRAPHICS] LFB not available. Calculating UMA address...\n");

    uint32_t candidate_addr = 0;

    if (total_mem > 0x00400000){
        candidate_addr = total_mem - 0x00300000; // 4MB VRAM

        fb_phys_addr = candidate_addr;
        fb_virt_addr = candidate_addr; // Identity map
        has_lfb = 1; // Считаем, что теперь у нас есть линейный доступ

        //com_puts("[GRAPHICS] UMA FB Phys: 0x");
        //com_puthex(fb_phys_addr);
        //com_puts(", Virt: 0x");
        //com_puthex(fb_virt_addr);
        //com_puts("\n");
        //com_puts("[GRAPHICS] Linear mode bit set in MSR. Direct access enabled.\n");
    } else {
        //com_puts("[GRAPHICS] ERROR: Not enough memory for UMA.\n");
    }
}



uint32_t graphics_get_fb_phys_addr(){
    return fb_phys_addr;//uma_video_base;
}


uint32_t graphics_get_fb_virt_addr(void){
    return fb_virt_addr;
}

int graphics_has_lfb(void){
    return has_lfb;
}



