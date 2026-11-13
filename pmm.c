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

#include "include-kernel/pmm.h"
#include <stddef.h>
#include "include-kernel/lib.h"   //вместо <string.h>
#include "include-kernel/comdebug.h"
#include "include-kernel/draw.h"
#include "include-kernel/debug.h"
#include "include-kernel/kernel_heap.h"


#define PMM_LOW_BOUND  0x100000u
#define PMM_HIGH_BOUND (pmm_get_total_memory())


//Глобальные переменные
static uint8_t *bitmap = NULL;
size_t total_pages = 0;
size_t free_pages = 0;
static uint32_t memory_start = 0;

uint32_t fb_virt_addr = 0;
uint32_t fb_phys_addr = 0;
int has_lfb = 0;

uint32_t min_addr = 0x100000;
uint32_t pmm_min_free_addr = 0;

//char _end;

void itoa2(int value, char *str, int base)
{
    char *ptr = str, *ptr1 = str, tmp_char;
    int tmp_value;

    do
    {
        tmp_value = value;
        value /= base;
        *ptr++ = "0123456789"[tmp_value - value * base];
    }
    while (value);

    *ptr-- = '\0';
    while (ptr1 < ptr)
    {
        tmp_char = *ptr;
        *ptr-- = *ptr1;
        *ptr1++ = tmp_char;
    }
}

void pmm_init(uint16_t num_entries, struct e820_entry *entries)
{

    //Фиксированный размер физической памяти
    //uintptr_t min_addr = 0x100000;          //1 МБ
    //uintptr_t max_addr = 1536 * 1024 * 1024; //128 МБ
    //Собираем реальный верхний край RAM из E820
    uint64_t max_phys = 0;
    for (uint16_t i = 0; i < num_entries; i++)
    {
        if (entries[i].type != 1) continue;
        uint64_t end = entries[i].base + entries[i].length;
        if (end > max_phys) max_phys = end;
    }

    //Отрезаем всё выше 3 ГБ (PCI hole) — это НЕ RAM
    if (max_phys > PCI_HOLE_START) max_phys = PCI_HOLE_START;
    //
    //if (max_phys > 0x40000000ull) max_phys = 0x40000000ull;   // 1 GB
    //
    uint32_t max_addr = (uint32_t)(max_phys & ~0xFFFull);



    //Если BIOS вернул меньше 16 МБ — это мусор
    if (max_addr <= min_addr + 16 * 1024 * 1024)
    {
        com_puts("[PMM] E820 BROKEN, max_phys=");
        com_puthex((uint32_t)max_phys);
        com_puts(" — fallback to 128 MB\n");
        max_addr = 0x100000 + 128 * 1024 * 1024;   //безопасный дефолт
    }

    uintptr_t total_mem = max_addr - min_addr;

    //bitmap покрывает всю физическую память от 0 до max_addr,
    //чтобы иметь возможность освобождать низкие адреса (образы модулей)
    memory_start = 0;
    total_pages = max_addr / PAGE_SIZE;

    extern char _kernel_end;
    uint32_t kernel_end_addr = ((uint32_t)&_kernel_end + 0xFFF) & ~0xFFFu;
    if (kernel_end_addr < 0x100000) kernel_end_addr = 0x200000;  // sanity

    bitmap = (uint8_t*)kernel_end_addr;
    size_t bitmap_size = (total_pages + 7) / 8;
    size_t bitmap_rounded = (bitmap_size + 0xFFF) & ~0xFFFu;
    mem_ptr = kernel_end_addr + bitmap_rounded;
    pmm_min_free_addr = mem_ptr;

    //Изначально всё свободно
    memset(bitmap, 0x00, bitmap_size);

    for (uint16_t i = 0; i < num_entries; i++)
    {
        if (entries[i].type != 1) continue;
        uint64_t base = entries[i].base;
        uint64_t end  = entries[i].base + entries[i].length;
        if (base < 0x100000) base = 0x100000;
        if (end  > max_addr) end  = max_addr;
        for (uint64_t a = base & ~0xFFFull; a < end; a += PAGE_SIZE)
        {
            size_t idx = a / PAGE_SIZE;
            if (idx < total_pages) bitmap[idx / 8] |= (1 << (idx % 8));
        }
    }

    //Явное резервирование BIOS-критичных регионов

    // 1) 0..0x10000 — IVT, BDA, BIOS data, EBDA (обычно внутри)
    for (uint32_t a = 0; a < 0x10000; a += PAGE_SIZE)
    {
        size_t idx = a / PAGE_SIZE;
        if (idx < total_pages) bitmap[idx / 8] &= ~(1 << (idx % 8));
    }

    // 2) 0xA0000..0x100000 — VGA framebuffer (A0000-BFFFF),
    //    Video BIOS (C0000-C7FFF), System BIOS ROM (E0000-FFFFF)
    //    Не отдаём ни при каких условиях.
    for (uint32_t a = 0xA0000; a < 0x100000; a += PAGE_SIZE)
    {
        size_t idx = a / PAGE_SIZE;
        if (idx < total_pages) bitmap[idx / 8] &= ~(1 << (idx % 8));
    }

    // 3) Ядро + bitmap + начало кучи kmalloc
    for (uint32_t a = 0x100000; a < mem_ptr; a += PAGE_SIZE)
    {
        size_t idx = a / PAGE_SIZE;
        if (idx < total_pages) bitmap[idx / 8] &= ~(1 << (idx % 8));
    }

    // 4) Область драйверов 12..16 MB
    for (uint32_t a = 0xC00000; a < 0x1000000; a += PAGE_SIZE)
    {
        size_t idx = a / PAGE_SIZE;
        if (idx < total_pages) bitmap[idx / 8] &= ~(1 << (idx % 8));
    }

    //Пересчёт свободных
    free_pages = 0;
    for (size_t i = 0; i < total_pages; i++)
    {
        if (bitmap[i / 8] & (1 << (i % 8))) free_pages++;
    }

    //Резерв LFB и всего, куда указывает fb_phys (boot_info->framebuffer)
    if (boot_info && boot_info->framebuffer != 0)
    {
        uint32_t fb_start = boot_info->framebuffer & ~0xFFF;
        uint32_t fb_end   = fb_start + (boot_info->pitch * boot_info->height + 0xFFF) & ~0xFFF;
        for (uint32_t a = fb_start; a < fb_end; a += PAGE_SIZE)
        {
            size_t idx = a / PAGE_SIZE;
            if (idx < total_pages) bitmap[idx / 8] &= ~(1 << (idx % 8));
        }
    }


}



//Выделение одной страницы
void *pmm_alloc_page(void)
{
    if (free_pages == 0) return NULL;

    size_t start_search_index = (16 * 1024 * 1024 - memory_start) / PAGE_SIZE;
    if (start_search_index >= total_pages) start_search_index = 0;

    for (size_t i = start_search_index; i < total_pages; i++)
    {
        if (bitmap[i / 8] & (1 << (i % 8)))
        {
            uint32_t addr = memory_start + i * PAGE_SIZE;
            if (addr < pmm_min_free_addr || addr >= PMM_HIGH_BOUND) continue;
            bitmap[i / 8] &= ~(1 << (i % 8));
            free_pages--;
            return (void*)addr;
        }
    }
    for (size_t i = 0; i < start_search_index; i++)
    {
        if (bitmap[i / 8] & (1 << (i % 8)))
        {
            uint32_t addr = memory_start + i * PAGE_SIZE;
            if (addr < pmm_min_free_addr || addr >= PMM_HIGH_BOUND) continue;
            bitmap[i / 8] &= ~(1 << (i % 8));
            free_pages--;
            return (void*)addr;
        }
    }
    return NULL;
}

//Освобождение страницы
void pmm_free_page(void *addr)
{
    if (!addr) return;
    uintptr_t addr_val = (uintptr_t)addr;

    //Одиночное освобождение страниц ниже 1 MB — запрещено
    //Нижняя память освобождается только через pmm_free_region() (которая умеет не трогать BIOS-регионы).
    if (addr_val < 0x100000) return;

    if (addr_val < memory_start || addr_val >= memory_start + total_pages * PAGE_SIZE) return;
    if (addr_val >= 0xC00000 && addr_val < 0x1000000) return;

    //if (!addr) return;
    //uintptr_t addr_val = (uintptr_t)addr;
    //if (addr_val < memory_start || addr_val >= memory_start + total_pages * PAGE_SIZE) return;
    // Драйвер-регион [12, 16] MB — никогда не освобождать
    //if (addr_val >= 0xC00000 && addr_val < 0x1000000) return;

    size_t page_index = (addr_val - memory_start) / PAGE_SIZE;
    if(page_index >= total_pages) return;
    //Если страница уже свободна, выходим, защищая free_pages от ложного инкремента
    //if(bitmap[page_index / 8] & (1 << (page_index % 8))) return;
    if (bitmap[page_index / 8] & (1 << (page_index % 8)))
    {
        // double-free!
        draw_hex_dword((uint32_t)addr, 20, 600);
        draw_hex_dword(free_pages,     20, 620);
        asm volatile("cli");
        for(;;);
    }
    bitmap[page_index / 8] |= (1 << (page_index % 8));
    free_pages++;
}


//Освобождает диапазон [addr, addr+size) в PMM.
//В отличие от pmm_free_page / pmm_free_pages, умеет освобождать
//страницы ниже 1 MB (там загрузчик держал образы модулей).
//Не освобождает BIOS-критичные регионы, даже если они попали в диапазон.
//Идемпотентна: повторный вызов для уже свободных страниц ничего не делает.
void pmm_free_region(uint32_t addr, uint32_t size)
{
    if (size == 0) return;

    uint32_t start = addr & ~0xFFFu;
    uint32_t end   = (addr + size + 0xFFF) & ~0xFFFu;
    if (end <= start) return;

    for (uint32_t a = start; a < end; a += PAGE_SIZE)
    {
        //BIOS-критичные регионы — никогда не освобождаем
        if (a < 0x10000) continue;                    //IVT/BDA/BIOS data
        if (a >= 0xA0000 && a < 0x100000) continue;   //VGA/ROM/BIOS
        if (a >= 0xC00000 && a < 0x1000000) continue; //область драйверов

        size_t idx = a / PAGE_SIZE;
        if (idx >= total_pages) break;

        uint8_t *byte = &bitmap[idx / 8];
        uint8_t mask  = 1u << (idx % 8);

        if (*byte & mask) continue;  //уже свободна (idempotent)

        *byte |= mask;
        free_pages++;
    }
}


//Выделение нескольких страниц (непрерывный блок)
void *pmm_alloc_pages(size_t count)
{
    if (count == 0 || free_pages < count) return NULL;

    for (size_t i = 0; i <= total_pages - count; i++)
    {
        int found = 1;
        for (size_t j = 0; j < count; j++)
        {
            if (!(bitmap[(i + j) / 8] & (1 << ((i + j) % 8))))
            {
                found = 0;
                break;
            }
        }
        if (found)
        {
            for (size_t j = 0; j < count; j++)
            {
                bitmap[(i + j) / 8] &= ~(1 << ((i + j) % 8));
            }
            free_pages -= count;
            return (void*)(memory_start + i * PAGE_SIZE);
        }
    }
    return NULL;
}

//Освобождение нескольких страниц
void pmm_free_pages(void *addr, size_t count)
{
    if (!addr || count == 0) return;
    uintptr_t addr_val = (uintptr_t)addr;
    if (addr_val < memory_start || addr_val >= memory_start + total_pages * PAGE_SIZE) return;
    size_t page_index = (addr_val - memory_start) / PAGE_SIZE;
    if (page_index + count > total_pages) return;

    for(size_t i = 0; i < count; i++)
    {
        bitmap[(page_index + i) / 8] |= (1 << ((page_index + i) % 8));
    }
    free_pages += count;
}

//Получить общий размер свободной памяти
size_t pmm_get_free_memory(void)
{
    return free_pages * PAGE_SIZE;
}

//Получить общий размер памяти
size_t pmm_get_total_memory(void)
{
    return total_pages * PAGE_SIZE;
}

/*
void graphics_init_uma_address(uint32_t total_mem, int lfb_available, uint32_t lfb_addr_from_boot){
    //com_init(); //Инициализируем COM сразу
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
}
*/
void graphics_init_uma_address(uint32_t total_mem, int lfb_available, uint32_t lfb_addr_from_boot)
{
    //Доверяем адресу от загрузчика, даже если флаг LFB в graphics_flags не выставлен
    //Причина: некоторые BIOS (в т.ч. VGABIOS-lgpl в Bochs) успешно активируют LFB,
    //но не выставляют/неправильно сообщают флаг VBE mode attributes
    //Если lfb_addr от stage2 ненулевой — значит, PhysBasePtr был валидным, и писать туда безопасно
    //Платформы без LFB (VGA text) присылают lfb_addr = 0 — их не задеваем
    if (lfb_addr_from_boot != 0)
    {
        fb_phys_addr = lfb_addr_from_boot;
        fb_virt_addr = lfb_addr_from_boot;
        has_lfb = 1;
        return;
    }

    //Fallback: попытка вычислить UMA
    (void)total_mem;
    (void)lfb_available;
}


uint32_t graphics_get_fb_phys_addr()
{
    return fb_phys_addr;//uma_video_base;
}


uint32_t graphics_get_fb_virt_addr(void)
{
    return fb_virt_addr;
}

int graphics_has_lfb(void)
{
    return has_lfb;
}



