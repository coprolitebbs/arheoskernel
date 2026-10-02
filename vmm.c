#include "include-kernel/vmm.h"
#include "include-kernel/pmm.h"
#include "include-kernel/lib.h"
#include "include-kernel/draw.h"
#include "include-kernel/debug.h"
#include "include-kernel/comdebug.h"
#include "include-kernel/kconsole.h"


static page_directory_t *kernel_dir = NULL;
page_directory_t *kernel_directory = NULL;
static uint32_t last_directory_phys = 0;

uint32_t test_buffer_phys_start = 0;

// Создание нового каталога страниц
page_directory_t* vmm_create_address_space(uint32_t *phys_out){
    uint32_t phys = (uint32_t)pmm_alloc_page();
    if(!phys) return NULL;
    page_directory_t *dir = (page_directory_t*)phys;
    memset(dir, 0, PAGE_SIZE);

    //Зеркально копируем первые 32 записи каталога ядра (все 128 МБ системной памяти)
    for(int t = 0; t < 32; t++){
        (*dir)[t] = (*kernel_directory)[t] & ~PAGE_USER;
    }
    //Для всех остальных индексов каталога (пользовательское пространство)
    for(int t = 32; t < 1024; t++){
        if ((*kernel_directory)[t] != 0){
            (*dir)[t] = (*kernel_directory)[t] | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
        }
    }

    *phys_out = phys;
    return dir;
}


void vmm_map_page(page_directory_t *dir, uint32_t virt, uint32_t phys, uint32_t flags){
    uint32_t pd_index = virt >> 22;
    uint32_t pt_index = (virt >> 12) & 0x3FF;

    uint32_t entry = (*dir)[pd_index];
    uint32_t *table;

    if (!(entry & PAGE_PRESENT)){
        uint32_t table_phys = (uint32_t)pmm_alloc_page();
        if (!table_phys) return;
        table = (uint32_t*)table_phys;
        memset(table, 0, PAGE_SIZE);
        (*dir)[pd_index] = table_phys | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    } else {
        table = (uint32_t*)(entry & 0xFFFFF000);
    }

    //Записываем физический адрес целевой страницы в таблицу страниц
    table[pt_index] = (phys & 0xFFFFF000) | flags;
}






// Удаление отображения
void vmm_unmap_page(page_directory_t *dir, uint32_t virt){
    uint32_t pd_index = virt >> 22;
    uint32_t pt_index = (virt >> 12) & 0x3FF;

    uint32_t entry = (*dir)[pd_index];
    if (!(entry & PAGE_PRESENT)) return;

    page_table_t *table = (page_table_t*)(entry & 0xFFFFF000);
    (*table)[pt_index] = 0;
}

//Получить физический адрес по виртуальному
uint32_t vmm_get_phys_addr(page_directory_t *dir, uint32_t virt){
    uint32_t pd_index = virt >> 22;
    uint32_t pt_index = (virt >> 12) & 0x3FF;

    uint32_t entry = (*dir)[pd_index];
    if (!(entry & PAGE_PRESENT)) return 0;

    page_table_t *table = (page_table_t*)(entry & 0xFFFFF000);
    uint32_t pt_entry = (*table)[pt_index];
    if (!(pt_entry & PAGE_PRESENT)) return 0;

    return (pt_entry & 0xFFFFF000) | (virt & 0xFFF);
}

// Переключение каталога
void vmm_switch_directory(uint32_t phys){
    asm volatile("mov %0,%%cr3"::"r"(phys & 0xFFFFF000):"memory");
}

//Инициализация VMM
void vmm_init(void){
    //com_puts("[VMM] Initializing...\n");
    kernel_directory = (page_directory_t*)pmm_alloc_page();

    if (!kernel_directory){
        //com_puts("[VMM] ERROR: Failed to alloc page directory!\n");
        for(;;);
    }
    memset(kernel_directory,0,PAGE_SIZE);
    //Identity map ядра, только первые 16 МБ пока. Без USER
    uint32_t phys = 0;
    for(uint32_t t = 0; t < 32; t++){
        page_table_t *table = (page_table_t*)pmm_alloc_page();
        if(!table) for(;;);
        memset(table, 0, PAGE_SIZE);
        for(int i = 0; i < 1024; i++){
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



void vmm_map_pages(uint32_t vbe_phys, uint32_t vbe_size){
    if(vbe_phys != 0 && vbe_size != 0){
        for(uint32_t offset = 0; offset < vbe_size; offset += PAGE_SIZE) {
            uint32_t p_addr = vbe_phys + offset;
            uint32_t v_addr = vbe_phys + offset;
            vmm_map_page(kernel_directory, v_addr, p_addr, PAGE_PRESENT | PAGE_WRITE);
        }
    }
}



//Выделить страницу в виртуальном адресном пространстве ядра (пока простая заглушка)
void* vmm_alloc_kernel_page(void) {
    // TODO: реализовать выделение виртуальной страницы
    return NULL;
}

void vmm_free_kernel_page(void *addr) {
    // TODO: реализовать освобождение
}


page_directory_t* vmm_create_user_space(void){
    page_directory_t *dir;
    dir=(page_directory_t*)pmm_alloc_page();
    if(!dir)
        return NULL;
    memset(dir,0,PAGE_SIZE);

    for(int i=0;i<4;i++){
        (*dir)[i]=(*kernel_directory)[i];
    }
    return dir;
}


page_directory_t* vmm_get_kernel_directory(void){
    return kernel_directory;
}


void vmm_copy_kernel_space(page_directory_t *dir){
    for(int i=0;i<1024;i++){
        (*dir)[i]=(*kernel_directory)[i];
    }
}

uint32_t vmm_get_last_directory_phys(void){
    return last_directory_phys;
}
