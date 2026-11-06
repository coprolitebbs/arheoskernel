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

#include "include-kernel/vmm.h"
#include "include-kernel/pmm.h"
#include "include-kernel/lib.h"
#include "include-kernel/draw.h"
#include "include-kernel/debug.h"
#include "include-kernel/comdebug.h"
#include "include-kernel/kconsole.h"
#include "include-kernel/task.h"

static page_directory_t *kernel_dir = NULL;
page_directory_t *kernel_directory = NULL;
static uint32_t last_directory_phys = 0;

uint32_t test_buffer_phys_start = 0;
uint32_t vmm_kernel_identity_pdes = 32;

static uint8_t vmm_critical_pdes[1024 / 8] = {0};


void vmm_mark_pde_critical(uint32_t pd_index)
{
    if (pd_index < 1024)
        vmm_critical_pdes[pd_index >> 3] |= (uint8_t)(1u << (pd_index & 7));
}

static int vmm_pde_is_critical(uint32_t pd_index)
{
    if (pd_index >= 1024) return 0;
    return (vmm_critical_pdes[pd_index >> 3] >> (pd_index & 7)) & 1;
}



//Уничтожает PD и все PT, которые ему принадлежат эксклюзивно
//PT, совпадающие с kernel_directory (шаринг kernel-PDE), не освобождаются
//PD == kernel_directory игнорируется (защита от идиота)
void vmm_destroy_address_space(page_directory_t *dir)
{
    if (!dir) return;
    if (dir == kernel_directory) return;
    if (!kernel_directory) return;

    for (uint32_t t = 0; t < 1024; t++)
    {
        uint32_t pde = (*dir)[t];
        if (pde == 0) continue;
        if (!(pde & PAGE_PRESENT)) continue;
        if (pde & 0x80) continue;                   //4MB page — PT нет

        //сравниваем физический адрес PT, а не весь PDE. PDE user-задачи может
        //отличаться от kernel-PDE флагами (например, апгрейд PAGE_USER в
        //vmm_map_page), но указывать на тот же kernel-PT
        if (((*kernel_directory)[t] & 0xFFFFF000) == (pde & 0xFFFFF000)) continue;

        uint32_t pt_phys = pde & 0xFFFFF000;
        if (pt_phys != 0)
        {
            pmm_free_page((void*)pt_phys);
            (*dir)[t] = 0;
        }
    }
    pmm_free_page((void*)dir);
}


//Заполняет один PDE в user-PD из kernel_directory, если тот пуст и не запрещён политикой
//(user-диапазон без critical). Для critical PDE создаёт свежую PT, чтобы изолировать
//user-PD от kernel_directory
//Идемпотентна: если PDE уже непустой — ничего не делает
void vmm_sync_pde_into_user_pd(page_directory_t *user_pd, uint32_t pd_index)
{
    if (!user_pd || user_pd == kernel_directory) return;
    if (pd_index >= 1024) return;

    uint32_t pde = (*kernel_directory)[pd_index];
    if (pde == 0) return;
    if (pde & PAGE_USER) return;              //user-PDE в kernel_dir быть не должно
    if ((*user_pd)[pd_index] != 0) return;    //уже заполнено

    //user-диапазон: без critical не трогаем
    if (pd_index >= 256 && pd_index <= 511 && !vmm_pde_is_critical(pd_index)) return;

    //4MB page — не PT, копируем как есть
    if (pde & 0x80)
    {
        (*user_pd)[pd_index] = pde & ~PAGE_USER;
        return;
    }

    //Critical user-PDE: свежая PT-копия для изоляции
    if (pd_index >= 256 && pd_index <= 511)
    {
        uint32_t old_pt_phys = pde & 0xFFFFF000;
        uint32_t new_pt_phys = (uint32_t)pmm_alloc_page();
        if (!new_pt_phys)
        {
            com_puts("[VMM] WARN: cannot alloc PT for critical PDE ");
            com_puthex(pd_index);
            com_puts("\n");
            return;
        }
        page_table_t *old_pt = (page_table_t*)old_pt_phys;
        page_table_t *new_pt = (page_table_t*)new_pt_phys;
        for (int i = 0; i < 1024; i++)
        {
            (*new_pt)[i] = (*old_pt)[i] & ~PAGE_USER;
        }
        (*user_pd)[pd_index] = new_pt_phys | (pde & 0xFFF);
        return;
    }

    //kernel-PDE 0..255 или 512..1023: шарим PT (не копируем)
    (*user_pd)[pd_index] = pde & ~PAGE_USER;
}

//Заполнить все kernel-PDE в user-PD. Используется при создании user-PD и при финальной синхронизации перед enqueue задачи
void vmm_sync_kernel_pdes(page_directory_t *user_pd)
{
    if (!user_pd || user_pd == kernel_directory) return;
    for (uint32_t t = 0; t < 1024; t++)
    {
        vmm_sync_pde_into_user_pd(user_pd, t);
    }
}

page_directory_t* vmm_create_address_space(uint32_t *phys_out)
{
    uint32_t phys = (uint32_t)pmm_alloc_page();
    if (!phys) return NULL;
    page_directory_t *dir = (page_directory_t*)phys;
    memset(dir, 0, PAGE_SIZE);
    vmm_sync_kernel_pdes(dir);
    *phys_out = phys;
    return dir;
}

void vmm_map_page(page_directory_t *dir, uint32_t virt, uint32_t phys, uint32_t flags)
{
    uint32_t pd_index = virt >> 22;
    uint32_t pt_index = (virt >> 12) & 0x3FF;
    uint32_t entry = (*dir)[pd_index];
    uint32_t *table;

    if (!(entry & PAGE_PRESENT))
    {
        uint32_t table_phys = (uint32_t)pmm_alloc_page();
        if (!table_phys) return;
        table = (uint32_t*)table_phys;
        memset(table, 0, PAGE_SIZE);
        uint32_t pd_flags = PAGE_PRESENT | PAGE_WRITE;
        if (flags & PAGE_USER) pd_flags |= PAGE_USER;
        (*dir)[pd_index] = table_phys | pd_flags;
        //kernel_directory только что получил НОВЫЙ PDE — зеркалим его во все user-PD,
        //чтобы ядро могло обращаться к этому VA из user-контекста (CR3 = user-PD)
        if (dir == kernel_directory)
        {
            task_mirror_kernel_pde(pd_index);
        }
    }
    else
    {
        uint32_t table_phys = entry & 0xFFFFF000;
        if (table_phys < 0x100000 || table_phys >= 0xC0000000) return;  //Испорчен PDE
        table = (uint32_t*)table_phys;

        //Если мапим USER-страницу в PDE, у которого нет PAGE_USER, апгрейдим PDE
        //Это нужно для critical user-PDE (LFB/MMIO), которые vmm_create_address_space копирует
        //без USER, чтобы защитить kernel-only PTE (сам LFB). USER на PDE + без USER
        //на PTE LFB -> Ring 3 до LFB не достанет; USER на PDE + USER на свежей странице -> Ring 3 работает
        if ((flags & PAGE_USER) && !(entry & PAGE_USER))
        {
            //Только если PT не шарится с kernel_directory,
            //Иначе выставили бы USER на kernel-PT (утечка ядра в Ring 3)
            if ((*kernel_directory)[pd_index] == entry)
            {
                com_puts("[VMM] FATAL: user map into shared kernel PDE ");
                com_puthex(pd_index);
                com_puts(" virt=");
                com_puthex(virt);
                com_puts("\n");

                return;   //отказываемся: user нельзя мапить в kernel PDE
            }
            (*dir)[pd_index] = entry | PAGE_USER;
        }
    }

    table[pt_index] = (phys & 0xFFFFF000) | flags;

    //Если пишем kernel-страницу в critical user-PDE, надо добавить её и в свежие PT копии всех user-PD (там PT — не шаренная)
    if (dir == kernel_directory && !(flags & PAGE_USER)
            && pd_index >= 256 && pd_index <= 511)
    {
        task_propagate_kernel_pte(pd_index, pt_index, table[pt_index]);
    }

    //TLB invalidate — только если мапим в активный CR3
    uint32_t active_cr3;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(active_cr3));

    if (dir == kernel_directory || (active_cr3 & 0xFFFFF000) == ((uint32_t)dir & 0xFFFFF000))
    {
        __asm__ __volatile__("invlpg (%0)" : : "r"(virt & 0xFFFFF000) : "memory");
    }
}


//Маппит MMIO-регион как некэшируемый (PCD + PWT). Нужно для регистров
//контроллеров (EHCI), чтобы CPU не читал устаревшие значения из кэша.
void vmm_map_pages_uncached(uint32_t phys, uint32_t size)
{
    if (phys == 0 || size == 0) return;
    for (uint32_t offset = 0; offset < size; offset += PAGE_SIZE)
    {
        uint32_t p_addr = phys + offset;
        uint32_t v_addr = phys + offset;
        vmm_map_page(kernel_directory, v_addr, p_addr,
                     PAGE_PRESENT | PAGE_WRITE | PAGE_PCD | PAGE_PWT);
    }
}


// Удаление отображения
void vmm_unmap_page(page_directory_t *dir, uint32_t virt)
{
    uint32_t pd_index = virt >> 22;
    uint32_t pt_index = (virt >> 12) & 0x3FF;

    uint32_t entry = (*dir)[pd_index];
    if (!(entry & PAGE_PRESENT)) return;

    //page_table_t *table = (page_table_t*)(entry & 0xFFFFF000);
    uint32_t table_phys = entry & 0xFFFFF000;
    if (table_phys < 0x100000 || table_phys >= 0xC0000000) return;  //порченый PDE

    //запрещаем unmap через PT, разделяемый с kernel_directory (для user-PD).
    //PT identity-областей и kernel heap шарится по значению PDE во все user-PD;
    //обнуление PTE в таком PT стирает mapping глобально для ядра и всех задач.
    //Защита от vm_pages-записей с мусорным virt (после порчи heap_next/структур).
    if (dir != kernel_directory && kernel_directory &&
            ((*kernel_directory)[pd_index] & 0xFFFFF000) == table_phys)
    {
        return;
    }

    page_table_t *table = (page_table_t*)table_phys;

    (*table)[pt_index] = 0;
    uint32_t cr3;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
    if (dir == kernel_directory || (cr3 & 0xFFFFF000) == ((uint32_t)dir & 0xFFFFF000))
    {
        __asm__ __volatile__("invlpg (%0)" : : "r"(virt & 0xFFFFF000) : "memory");
    }
}

//Получить физический адрес по виртуальному
uint32_t vmm_get_phys_addr(page_directory_t *dir, uint32_t virt)
{
    if (!dir) return 0;
    uint32_t pd_index = virt >> 22;
    uint32_t pt_index = (virt >> 12) & 0x3FF;
    uint32_t entry = (*dir)[pd_index];
    if (!(entry & PAGE_PRESENT)) return 0;
    //< 1 MB или PCI hole — порча PDE
    uint32_t table_phys = entry & 0xFFFFF000;
    if (table_phys < 0x100000 || table_phys >= 0xC0000000) return 0;
    page_table_t *table = (page_table_t*)table_phys;
    uint32_t pt_entry = (*table)[pt_index];
    if (!(pt_entry & PAGE_PRESENT)) return 0;
    return (pt_entry & 0xFFFFF000) | (virt & 0xFFF);
}

// Переключение каталога
void vmm_switch_directory(uint32_t phys)
{
    asm volatile("mov %0,%%cr3"::"r"(phys & 0xFFFFF000):"memory");
}

//Инициализация VMM
void vmm_init(void)
{
    //com_puts("[VMM] Initializing...\n");
    kernel_directory = (page_directory_t*)pmm_alloc_page();

    if (!kernel_directory)
    {
        //com_puts("[VMM] ERROR: Failed to alloc page directory!\n");
        for(;;);
    }
    memset(kernel_directory,0,PAGE_SIZE);

    uint32_t total_mb = pmm_get_total_memory() / (1024 * 1024);
    //draw_hex_dword(total_mb, 120, 420);for(;;);
    uint32_t pd_count = (total_mb + 3) / 4;   // 4 МБ на один PDE
    if (pd_count > 896) pd_count = 896;       //оставляем место под kernel heap/FB
    //uint32_t cover_to = pd_count * 0x400000;
    //uint32_t pmm_top = min_addr + (uint32_t)total_pages * PAGE_SIZE;
    //if (cover_to < pmm_top) {
    //pd_count = (pmm_top + 0x3FFFFF) / 0x400000;  // округлить вверх
    //}
    vmm_kernel_identity_pdes = pd_count;
    //Identity map ядра, только первые 16 МБ пока. Без USER
    uint32_t phys = 0;
    for(uint32_t t = 0; t < /*32*/pd_count; t++)
    {
        page_table_t *table = (page_table_t*)pmm_alloc_page();
        if(!table) for(;;);
        memset(table, 0, PAGE_SIZE);
        for(int i = 0; i < 1024; i++)
        {
            //Identity mapping: виртуальный адрес равен физическому
            (*table)[i] = phys | PAGE_PRESENT | PAGE_WRITE;
            phys += PAGE_SIZE;
        }
        //Записываем таблицу в каталог страниц ядра
        (*kernel_directory)[t] = (uint32_t)table | PAGE_PRESENT | PAGE_WRITE;
    }

    //включаем paging
    //com_puts("[VMM] Enabling paging...\n");
    asm volatile("mov %0,%%cr3"::"r"(kernel_directory));
    uint32_t cr0;
    asm volatile("mov %%cr0,%0":"=r"(cr0));
    cr0 |= 0x80000000;
    asm volatile("mov %0,%%cr0"::"r"(cr0));
    //com_puts("[VMM] Paging enabled. Hello from protected mode with paging!\n");
}



void vmm_map_pages(uint32_t vbe_phys, uint32_t vbe_size)
{
    if(vbe_phys != 0 && vbe_size != 0)
    {
        for(uint32_t offset = 0; offset < vbe_size; offset += PAGE_SIZE)
        {
            uint32_t p_addr = vbe_phys + offset;
            uint32_t v_addr = vbe_phys + offset;
            //LFB/MMIO должен быть доступен ядру и из контекста user-задачи (CR3 = user-PD)
            //Помечаем его PDE как critical — иначе vmm_create_address_space() выкинет
            //его из user-PD на машинах, где LFB попал в 256..511 (типично для Geode: 0x40000000..0x7FFFFFFF)
            vmm_mark_pde_critical(v_addr >> 22);
            vmm_map_page(kernel_directory, v_addr, p_addr, PAGE_PRESENT | PAGE_WRITE);
        }
    }
}

page_directory_t* vmm_create_user_space(void)
{
    page_directory_t *dir;
    dir=(page_directory_t*)pmm_alloc_page();
    if(!dir)
        return NULL;
    memset(dir,0,PAGE_SIZE);

    for(int i=0; i<4; i++)
    {
        (*dir)[i]=(*kernel_directory)[i];
    }

    for (int t = 768; t < 1024; t++)
    {
        if ((*kernel_directory)[t]) (*dir)[t]=(*kernel_directory)[t];
    }

    return dir;
}


page_directory_t* vmm_get_kernel_directory(void)
{
    return kernel_directory;
}


void vmm_copy_kernel_space(page_directory_t *dir)
{
    for(int i=0; i<1024; i++)
    {
        (*dir)[i]=(*kernel_directory)[i];
    }
}

uint32_t vmm_get_last_directory_phys(void)
{
    return last_directory_phys;
}
