#include "include-kernel/vmm.h"
#include "include-kernel/pmm.h"
#include "include-kernel/lib.h"
#include "include-kernel/draw.h"
#include "include-kernel/debug.h"




static page_directory_t *kernel_dir = NULL;
page_directory_t *kernel_directory = NULL;
static uint32_t last_directory_phys=0;

// Создание нового каталога страниц
page_directory_t* vmm_create_address_space(uint32_t *phys_out)
{
    uint32_t phys=(uint32_t)pmm_alloc_page();

    if(!phys)
        return NULL;

    page_directory_t *dir=(page_directory_t*)phys;

    memcpy(
        dir,
        kernel_directory,
        PAGE_SIZE
    );

	for(uint32_t i=0;i<768;i++)
	{
		if(i!=0 && i!=1) (*dir)[i]=0;
	}


	(*dir)[0] &= ~PAGE_USER;
	(*dir)[1] &= ~PAGE_USER;



	//memset(dir,0,PAGE_SIZE);

	//for(int i=768;i<1024;i++) (*dir)[i]=(*kernel_directory)[i];


    *phys_out=phys;



    return dir;
}

// Отображение страницы
void vmm_map_page(page_directory_t *dir,uint32_t virt,uint32_t phys,uint32_t flags){


    uint32_t pd_index = virt >> 22;
    uint32_t pt_index = (virt >> 12) & 0x3FF;

    uint32_t entry = (*dir)[pd_index];

    page_table_t *table;

    if(!(entry & PAGE_PRESENT))
    {
        uint32_t table_phys = (uint32_t)pmm_alloc_page();

        if(!table_phys)
            return;

        table = (page_table_t*)table_phys;

        memset(
            table,
            0,
            PAGE_SIZE
        );


        (*dir)[pd_index] = table_phys | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
		//(*table)[pt_index] = (phys & 0xFFFFF000) | PAGE_PRESENT | flags;


    }
    else
    {
        table =
            (page_table_t*)(entry & 0xFFFFF000);


        (*dir)[pd_index] |= PAGE_USER;
    }


    (*table)[pt_index] =
        (phys & 0xFFFFF000) |
        PAGE_PRESENT |
        PAGE_WRITE |
        PAGE_USER;
}

// Удаление отображения
void vmm_unmap_page(page_directory_t *dir, uint32_t virt) {
    uint32_t pd_index = virt >> 22;
    uint32_t pt_index = (virt >> 12) & 0x3FF;

    uint32_t entry = (*dir)[pd_index];
    if (!(entry & PAGE_PRESENT)) return;

    page_table_t *table = (page_table_t*)(entry & 0xFFFFF000);
    (*table)[pt_index] = 0;  // очищаем запись
}

// Получить физический адрес по виртуальному
uint32_t vmm_get_phys_addr(page_directory_t *dir, uint32_t virt) {
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
    asm volatile(
        "mov %0,%%cr3"
        :
        :"r"(phys & 0xFFFFF000)
        :"memory"
    );
}

// Инициализация VMM
void vmm_init(uint32_t vbe_phys){
    kernel_directory = (page_directory_t*)pmm_alloc_page();

    if (!kernel_directory)
        for(;;);

    memset(kernel_directory,0,PAGE_SIZE);


    /*
        1. Identity map ядра
        Только первые 16 МБ пока.
        Без USER!
    */

    uint32_t phys = 0;


    for(uint32_t t=0;t<4;t++)
    {
        page_table_t *table =
            (page_table_t*)pmm_alloc_page();

        if(!table)
            for(;;);


        memset(table,0,PAGE_SIZE);


        for(int i=0;i<1024;i++)
        {
            (*table)[i] =
                phys |
                PAGE_PRESENT |
                PAGE_WRITE;

            phys += PAGE_SIZE;
        }


        (*kernel_directory)[t] =
            (uint32_t)table |
            PAGE_PRESENT |
            PAGE_WRITE;
    }



    /*
       2. VBE framebuffer только kernel
    */

    if(vbe_phys)
    {
        uint32_t size=3*1024*1024;

        for(uint32_t i=0;
            i<size;
            i+=PAGE_SIZE)
        {
            vmm_map_page(
                kernel_directory,
                vbe_phys+i,
                vbe_phys+i,
                PAGE_PRESENT|PAGE_WRITE
            );
        }
    }



    /*
       включаем paging
    */


    asm volatile(
        "mov %0,%%cr3"
        :
        :"r"(kernel_directory)
    );


    uint32_t cr0;

    asm volatile(
        "mov %%cr0,%0"
        :"=r"(cr0)
    );


    cr0 |= 0x80000000;


    asm volatile(
        "mov %0,%%cr0"
        :
        :"r"(cr0)
    );
}

// Выделить страницу в виртуальном адресном пространстве ядра (пока простая заглушка)
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
    /*
       Копируем ядро
       в верхнюю часть адресного пространства

       пока ничего не копируем,
       потому что ядро ниже 16МБ

       позже перенесем в 0xC0000000
    */
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

uint32_t vmm_get_last_directory_phys(void)
{
    return last_directory_phys;
}
